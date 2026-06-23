#include <zephyr/kernel.h>
#include <soc.h>

#include "util.h"
#include "leds.h"
#include "saadc.h"
#include "pwm.h"
#include "emmc.h"
#include "usb.h"
#include "codec.h"
#include "audio.h"
#include "disk.h"

/* On any fatal error (incl. stack-overflow fault with HW_STACK_PROTECTION):
 * light all 8 LEDs solid and feed the WDT forever, so a crash is visible. */
void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	(void)reason; (void)esf;
	for (int i = 0; i < NUM_PB_LEDS; i++) set_pb_on(i);
	for (int i = 0; i < 4; i++) set_trk_on(i);
	for (;;) feed_wdt();
}

/* ── Power ─────────────────────────────────────────────────────────────────── */

static void enter_system_off(void)
{
	all_pb_off();
	NRF_PWM0->TASKS_STOP = 1;
	feed_wdt();
	NRF_POWER->RESETREAS = 0xFFFFFFFF;
	NRF_POWER->SYSTEMOFF  = 1;
	__DSB();
	for (;;);
}

/* Headphone level via CS42L42 mixer volume (MIXER_CHA/CHB_VOL). 0x00 = 0 dB
 * (full scale — too loud for headphones); attenuation grows with the code.
 * Level 0 = silence (handled by muting HP_CTL, since the mixer can't fully
 * mute); levels 1..7 cap well below full scale. Tune by ear. */
static const uint8_t vol_hp[8] = {0x00, 0x1D, 0x1A, 0x17, 0x14, 0x11, 0x0D, 0x0A};

/* Apply a 0..7 volume level to the headphone output: level 0 mutes via HP_CTL,
 * higher levels unmute and set the mixer attenuation. */
static void hp_apply_level(int lvl)
{
	if (lvl <= 0) {
		codec_headphone_mute(true);
	} else {
		codec_headphone_volume(vol_hp[lvl]);
		codec_headphone_mute(false);
	}
}

/* Decode the AIN0 track-button ladder into a bitmask of held stems (bit s = stem
 * s). Measured on-device: singles + all pairs are distinct, gaps ≥80 counts, so
 * a ±50 nearest-match is unambiguous. Triples/quad are unmeasured → return 0
 * (no solo) rather than a wrong guess. Returns 0 for idle / Play / unknown. */
static uint8_t track_mask(int al)
{
	static const struct { int16_t v; uint8_t m; } tbl[] = {
		{ 207, 0x1 }, { 401, 0x2 }, { 576, 0x3 }, { 725, 0x4 }, { 860, 0x5 },
		{ 981, 0x6 }, { 1211, 0x8 }, { 1307, 0x9 }, { 1390, 0xA }, { 1547, 0xC },
	};
	if (al < 120) return 0;
	for (unsigned i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++) {
		int d = al - tbl[i].v;
		if (d < 0) d = -d;
		if (d <= 50) return tbl[i].m;
	}
	return 0;
}

/* ── Stem faders ─────────────────────────────────────────────────────────────
 * The faders drive per-stem mix gain and must respond in real time. The audio
 * feed thread is cooperative and spends ~40% of wall-clock in long bit-bang
 * eMMC reads, during which the (lower-priority) main thread can't run — so
 * sampling the faders in main makes them freeze in ~68 ms chunks. Instead a
 * small thread, priority ABOVE the feed thread, samples the faders at ~125 Hz;
 * it preempts the feed even mid-read (a ~150 µs blip the deep TX queue absorbs)
 * so gain tracks the faders with no audible lag. */
#define FADER_MIN 120   /* raw ADC at bottom of travel → gain 0 */
#define FADER_MAX 3100  /* raw ADC at top of travel    → gain 256 (unity) */
#define VU_REF    171   /* baked level 0..255 that fills the 4-bar pb meter */

/* Shared UI state: the buttons (main thread) write these, the UI thread reads
 * them to render the pb-LED meter. */
static volatile int s_vol_level   = 3;   /* current 0..7 volume level */
static volatile int s_meter_ticks = 0;   /* >0 = show the volume bar (UI counts down) */

/* Stem on/off (track-button tap) + momentary solo (track-button hold), folded
 * into the fader gains by the UI thread. s_solo_mask: 0 = no solo, else a bit
 * per stem that is currently soloed (only those play). Multiple stems can be
 * muted at once (independent taps). */
static volatile uint8_t s_stem_muted[4] = {0, 0, 0, 0};
static volatile uint8_t s_solo_mask = 0;

K_THREAD_STACK_DEFINE(s_ui_stack, 1024);
static struct k_thread s_ui_thread;

/* UI thread: samples the 4 faders and renders both LED visualizers at ~125 Hz.
 * Runs ABOVE the audio feed thread so it preempts the feed's long eMMC reads —
 * the faders and the meters stay real-time instead of freezing in ~68 ms chunks
 * (which is what happened when this ran in the read-starved main thread). Its
 * per-tick work is ~0.2 ms, a blip the deep TX queue absorbs. */
static void ui_main(void *a, void *b, void *c)
{
	(void)a; (void)b; (void)c;
	/* saadc pselp per fader (= AIN+1): F1=AIN3=4 (stem0), F2=AIN6=7 (stem1),
	 * F3=AIN2=3 (stem2), F4=AIN7=8 (stem3). */
	static const uint8_t fader_ch[4] = {4u, 7u, 3u, 8u};
	int vu_disp = 0;
	int trk_disp[4] = {0, 0, 0, 0};
	/* Smooth real-time playback position for the meters. The feed thread's
	 * s_cur_block freezes for ~68 ms during each eMMC read then jumps, which
	 * makes the meters stutter. ui_blk advances at the audio rate (375 blk/s →
	 * 3 blocks per 8 ms tick) and is gently eased toward the real position to
	 * stay aligned and to follow song changes / seeks. */
	int32_t ui_blk = 0;

	uint16_t gains[4] = {256, 256, 256, 256};
	int fi = 0;
	/* Track-button state (sensed here at 125 Hz so quick taps aren't dropped).
	 * Tap = toggle the held stem(s); hold >250 ms = solo them until release.
	 * Both work with multiple buttons at once (see track_mask). */
	bool    trk_held    = false;  /* any track button down */
	bool    trk_soloed  = false;  /* this hold has activated solo */
	uint8_t trk_last    = 0;      /* last non-zero held mask (for tap-toggle) */
	int64_t trk_press_ms = 0;

	while (1) {
		/* Faders → per-stem mix gain. Sample ONE fader per tick (round-robin):
		 * each fader updates every 4 ticks ≈ 32 ms, still real-time. Hold the
		 * previous gain on a failed read (no stutter). With oversampling bypassed
		 * a read is ~20 µs, cheap enough to do at this high priority. */
		int raw = saadc_read(fader_ch[fi]);
		if (raw >= 0) {
			int t = (raw - FADER_MIN) * 256 / (FADER_MAX - FADER_MIN);
			if (t < 0)   t = 0;
			if (t > 256) t = 256;
			gains[fi] = (uint16_t)t;
		}
		fi = (fi + 1) & 3;

		/* Track buttons on AIN0 → bitmask of held stems. Tap toggles them; a
		 * hold >250 ms solos them, and the solo set follows the held buttons
		 * live (add/remove a finger to change it). */
		uint8_t cur = track_mask(saadc_read(1u));
		if (cur) {
			if (!trk_held) { trk_press_ms = k_uptime_get(); trk_soloed = false; trk_held = true; }
			trk_last = cur;
			if (!trk_soloed && k_uptime_get() - trk_press_ms > 250) trk_soloed = true;
			if (trk_soloed) s_solo_mask = cur;          /* live solo of the held set */
		} else if (trk_held) {                              /* all released */
			if (trk_soloed) {
				s_solo_mask = 0;
			} else {
				for (int s = 0; s < 4; s++)
					if (trk_last & (1u << s)) s_stem_muted[s] ^= 1u;
			}
			trk_held = false;
		}

		/* Fold in stem on/off + solo: while any stem is soloed, only soloed stems
		 * play; otherwise muted stems are silenced. Fader gain is the base. */
		uint16_t eff[4];
		uint8_t solo = s_solo_mask;
		for (int s = 0; s < 4; s++) {
			if (solo)  eff[s] = (solo & (1u << s)) ? gains[s] : 0;
			else       eff[s] = s_stem_muted[s] ? 0 : gains[s];
		}
		audio_set_stem_gains(eff);

		bool playing = audio_is_playing();

		/* Advance the smooth position at the audio rate, then ease toward the
		 * real (bursty) block to correct drift and follow song changes/seeks. */
		if (playing) ui_blk += 3;
		ui_blk += ((int32_t)audio_cur_block() - ui_blk) >> 4;
		if (ui_blk < 0) ui_blk = 0;
		uint32_t blk = (uint32_t)ui_blk;

		/* pb LEDs: volume bar for ~1 s after a vol button, else the live overall
		 * VU (stems × fader gains), else off. One-pole for a smooth envelope. */
		int fill;
		if (s_meter_ticks > 0) {
			s_meter_ticks--;
			fill = s_vol_level * NUM_PB_LEDS * PWM_TOP / 7;
		} else if (playing) {
			int target = (int)audio_vu_level_at(blk) * (NUM_PB_LEDS * PWM_TOP) / VU_REF;
			if (target > NUM_PB_LEDS * PWM_TOP) target = NUM_PB_LEDS * PWM_TOP;
			vu_disp += (target - vu_disp) / 2;
			fill = vu_disp;
		} else {
			vu_disp = 0;
			fill = 0;
		}
		for (int s = 0; s < NUM_PB_LEDS; s++) {
			int b = fill - s * PWM_TOP;
			if (b < 0)       b = 0;
			if (b > PWM_TOP) b = PWM_TOP;
			pwm1_set_duty(NUM_PB_LEDS - 1 - s, (uint16_t)b);
		}

		/* Track LEDs: per-stem baked level (× fader gain), one-pole smoothed. */
		for (int s = 0; s < 4; s++) {
			int target = playing ? (int)audio_stem_level_at(blk, s) * PWM_TOP / 255 : 0;
			trk_disp[s] += (target - trk_disp[s]) / 2;
			pwm0_set_duty(s, (uint16_t)trk_disp[s]);
		}

		k_msleep(8);
	}
}

/* ── Main ──────────────────────────────────────────────────────────────────── */

int main(void)
{
	/* Drop the main thread BELOW the audio feed thread (preempt 5) so the feed
	 * is never starved by main's button/USB/jack work. Main only needs to run
	 * often enough for input + the watchdog; the feed yields to it whenever its
	 * TX alloc blocks. (Nothing else runs yet, so lowering now is safe.) */
	k_thread_priority_set(k_current_get(), 10);

	NRF_PPI->CHENCLR = 0xFFFFFFFFUL;

	/* P1.10 = PIN_BTN_COM: power rail for button ladders and faders */
	NRF_P1->PIN_CNF[10] = GPIO_OUT_CNF;
	NRF_P1->OUTSET      = (1u << 10);

	/* P0.27 = function button: active-low, pull-up */
	NRF_P0->PIN_CNF[27] =
		(GPIO_PIN_CNF_DIR_Input     << GPIO_PIN_CNF_DIR_Pos)   |
		(GPIO_PIN_CNF_PULL_Pullup   << GPIO_PIN_CNF_PULL_Pos)  |
		(GPIO_PIN_CNF_INPUT_Connect << GPIO_PIN_CNF_INPUT_Pos);

	leds_init();
	saadc_init();
	pwm0_init();
	pwm1_init();   /* pb_leds via PWM1 (dimmable) — before any set_pb_on() */
	usb_cdc_init();

	/* eMMC init: pb_led[0] solid = success.
	 * On failure: pb_led[1] blinks N times = which init step failed (1-9). */
	if (!emmc_init()) {
		uint8_t step = emmc_fail_step();
		for (;;) {
			for (uint8_t i = 0; i < step; i++) {
				set_pb_on(1);
				delay_ms(200);
				all_pb_off();
				delay_ms(200);
			}
			delay_ms(1000);
			feed_wdt();
		}
	}

	feed_wdt();

	/* Audio codec bring-up. Register state available over USB via CODEC_DIAG. */
	bool codec_ok = codec_init();
	feed_wdt();

	/* Scan disk for the first valid song before starting I2S (no concurrent DMA). */
	static disk_header_t s_dh;
	static disk_song_entry_t s_se;
	uint32_t song_block_start = 0, song_block_count = 0;
	uint16_t first_song_idx = 0, total_songs = 0;
	bool song_found = false;
	if (disk_read_header(&s_dh) && s_dh.song_count > 0) {
		total_songs = s_dh.song_count;
		for (uint16_t i = 0; i < s_dh.song_count; i++) {
			if (disk_read_song(i, &s_se) && s_se.name[0] != '\0') {
				song_found       = true;
				first_song_idx   = i;
				song_block_start = s_se.block_start;
				song_block_count = s_se.block_count;
				break;
			}
		}
	}
	feed_wdt();

	/* I2S bring-up + ADPCM playback. Starts paused; play button toggles. */
	if (codec_ok && audio_init()) {
		if (song_found) {
			audio_set_source(AUDIO_SRC_ADPCM);
			audio_set_playlist(total_songs, first_song_idx);
			audio_set_levels_enabled(s_dh.version >= 2);  /* baked VU on v2 discs */
			audio_load_song(song_block_start, song_block_count);
		}
	}
	feed_wdt();

	/* Play button on ladder 1 (AIN0): measured idle≈0, play≈1808, track1≈210.
	 * Detect a press as a window around 1808; toggle play/pause on the edge. */
	int play_prev = 0;

	/* Speaker volume: 8 levels (0 = mute … 7 = loud) → TAS2505 P1/R46 attenuation
	 * (0x00 = 0 dB loudest, larger = quieter). Vol +/- on ladder 2 (AIN1):
	 * measured idle≈0, vol+≈1806, vol-≈729. Boot at a night-friendly level. */
	static const uint8_t vol_r46[8] = {0x7F, 0x48, 0x3C, 0x30, 0x24, 0x18, 0x0C, 0x00};
	int volup_prev = 0;
	int voldn_prev = 0;
	int volup_hold = 0;   /* loops a vol button has been held (for auto-repeat) */
	int voldn_hold = 0;
	int next_prev  = 0;
	int prev_prev  = 0;
	/* vol_level + meter_ticks live at file scope (s_vol_level / s_meter_ticks):
	 * the buttons here write them, the UI thread reads them to render the meter.
	 * The LED visualizers are drawn by the UI thread, not this loop. */

	/* Output routing: speaker (TAS2505) or headphones (CS42L42), chosen by the
	 * CS42L42 jack-sense. Exactly one is unmuted. Debounce jack reads so a noisy
	 * insert doesn't flap. Detect the initial state before the loop so booting
	 * with headphones in starts on the right output. */
	bool hp_out = codec_headphones_present();
	int  hp_raw_prev = hp_out ? 1 : 0;   /* last raw jack read */
	int  hp_debounce = 0;                /* consecutive stable raw reads */
	if (hp_out) {
		codec_speaker_mute(true);
		hp_apply_level(s_vol_level);
	} else {
		codec_speaker_volume(vol_r46[s_vol_level]);
	}

	/* Spawn the UI thread (faders + LED visualizers) at a priority ABOVE the
	 * audio feed thread (K_PRIO_COOP(NUM_COOP-1)) so it preempts the feed's long
	 * eMMC reads and keeps the faders and meters real-time. */
	k_thread_create(&s_ui_thread, s_ui_stack, K_THREAD_STACK_SIZEOF(s_ui_stack),
			ui_main, NULL, NULL, NULL,
			K_PRIO_PREEMPT(1), 0, K_NO_WAIT);
	k_thread_name_set(&s_ui_thread, "ui");

	while (1) {
		if (!(NRF_P0->IN & (1u << 27)))
			enter_system_off();

		bool uploading = usb_upload_active();

		/* Play/pause button (edge-triggered) on ladder 1 (AIN0). */
		int ladder = saadc_read(1u);  /* AIN0 */
		audio_dbg_set_ain0((uint16_t)ladder);   /* expose for threshold tuning via rome audio */
		int play_now = (ladder >= 1650 && ladder <= 1980);
		if (play_now && !play_prev)
			audio_toggle();
		play_prev = play_now;
		/* Track buttons (same AIN0 ladder) are sensed in the UI thread so quick
		 * taps aren't dropped while main is starved during eMMC reads. */

		/* Ladder 2 (AIN1): prev≈399, vol-≈729, next≈1207, vol+≈1806. */
		int vladder = saadc_read(2u);  /* AIN1 */
		int volup_now = (vladder >= 1620 && vladder <= 1960);
		int next_now  = (vladder >= 1080 && vladder <= 1340);
		int voldn_now = (vladder >=  620 && vladder <=  860);
		int prev_now  = (vladder >=  300 && vladder <=  520);
		/* Vol +/- with hold-to-repeat: step once on the press edge, then after a
		 * short hold (~180 ms) auto-repeat every ~70 ms while held. */
		enum { VOL_HOLD_DELAY = 10, VOL_HOLD_RATE = 4 };
		int vol_step = 0;
		if (volup_now) {
			if (!volup_prev) { volup_hold = 0; vol_step = 1; }
			else if (++volup_hold >= VOL_HOLD_DELAY && volup_hold % VOL_HOLD_RATE == 0) vol_step = 1;
		} else volup_hold = 0;
		if (voldn_now) {
			if (!voldn_prev) { voldn_hold = 0; vol_step = -1; }
			else if (++voldn_hold >= VOL_HOLD_DELAY && voldn_hold % VOL_HOLD_RATE == 0) vol_step = -1;
		} else voldn_hold = 0;
		if (vol_step > 0 && s_vol_level < 7) s_vol_level++;
		else if (vol_step < 0 && s_vol_level > 0) s_vol_level--;
		else vol_step = 0;
		if (vol_step != 0) {
			if (hp_out) hp_apply_level(s_vol_level);
			else        codec_speaker_volume(vol_r46[s_vol_level]);
			s_meter_ticks = 120;   /* ~1 s of volume bar at the UI thread's 8 ms tick */
		}
		/* Prev/next rocker: skip song + ensure playing. */
		if (next_now && !next_prev) { audio_skip(1);  audio_play(); }
		if (prev_now && !prev_prev) { audio_skip(-1); audio_play(); }
		volup_prev = volup_now;
		voldn_prev = voldn_now;
		next_prev  = next_now;
		prev_prev  = prev_now;

		/* (Faders + both LED visualizers are handled by the UI thread so they
		 * stay real-time during the feed thread's long eMMC reads.) */

		/* Headphone jack sense (every loop ≈ every 18 ms; the I2C read is cheap).
		 * Debounce: 2 stable raw reads before flipping output, so a noisy insert
		 * can't flap but the switch still feels immediate. On a switch, mute the
		 * old output and unmute the new at the current level so loudness stays
		 * roughly consistent across the change. */
		{
			int hp_raw = codec_headphones_present() ? 1 : 0;
			if (hp_raw == hp_raw_prev) {
				if (hp_debounce < 2) hp_debounce++;
			} else {
				hp_debounce = 0;
				hp_raw_prev = hp_raw;
			}
			if (hp_debounce >= 2 && hp_raw != (hp_out ? 1 : 0)) {
				hp_out = hp_raw;
				if (hp_out) {
					codec_speaker_mute(true);
					hp_apply_level(s_vol_level);
				} else {
					codec_headphone_mute(true);
					codec_speaker_volume(vol_r46[s_vol_level]);
					codec_speaker_mute(false);
				}
			}
		}

		/* (The pb + track LED visualizers are rendered by the UI thread.) */
		bool playing = audio_is_playing();

		/* Renode mirror: expose GPIO state for emulator observation */
		*(volatile uint32_t *)0x2000FFF0 = NRF_P0->OUT;
		*(volatile uint32_t *)0x2000FFF4 = NRF_P1->OUT;

		usb_cdc_poll();

		feed_wdt();
		if (!uploading)
			k_msleep(playing ? 18 : 30);   /* faster while playing → smoother VU */

		if (!(NRF_P0->IN & (1u << 27)))
			enter_system_off();
	}

	return 0;
}
