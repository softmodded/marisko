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

/* ── Main ──────────────────────────────────────────────────────────────────── */

int main(void)
{
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
	int vol_level  = 3;
	int volup_prev = 0;
	int voldn_prev = 0;
	int volup_hold = 0;   /* loops a vol button has been held (for auto-repeat) */
	int voldn_hold = 0;
	int next_prev  = 0;
	int prev_prev  = 0;
	int meter_ticks = 0;   /* loops left to show the volume bar before bounce resumes */

	/* Audio VU: reference for a full 4-bar bar (higher = less sensitive) and a
	 * displayed envelope with instant attack + smooth time-based decay. */
	const int VU_REF = 171;   /* baked level 0..255 that fills 4 bars (≈22000>>7) */
	int vu_disp = 0;

	/* Output routing: speaker (TAS2505) or headphones (CS42L42), chosen by the
	 * CS42L42 jack-sense. Exactly one is unmuted. Debounce jack reads so a noisy
	 * insert doesn't flap. Detect the initial state before the loop so booting
	 * with headphones in starts on the right output. */
	bool hp_out = codec_headphones_present();
	int  hp_raw_prev = hp_out ? 1 : 0;   /* last raw jack read */
	int  hp_debounce = 0;                /* consecutive stable raw reads */
	if (hp_out) {
		codec_speaker_mute(true);
		hp_apply_level(vol_level);
	} else {
		codec_speaker_volume(vol_r46[vol_level]);
	}

	while (1) {
		if (!(NRF_P0->IN & (1u << 27)))
			enter_system_off();

		bool uploading = usb_upload_active();

		/* Play/pause button (edge-triggered) on ladder 1 (AIN0). */
		int ladder = saadc_read(1u);  /* AIN0 */
		int play_now = (ladder >= 1650 && ladder <= 1980);
		if (play_now && !play_prev)
			audio_toggle();
		play_prev = play_now;

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
		if (vol_step > 0 && vol_level < 7) vol_level++;
		else if (vol_step < 0 && vol_level > 0) vol_level--;
		else vol_step = 0;
		if (vol_step != 0) {
			if (hp_out) hp_apply_level(vol_level);
			else        codec_speaker_volume(vol_r46[vol_level]);
			meter_ticks = 40;
		}
		/* Prev/next rocker: skip song + ensure playing. */
		if (next_now && !next_prev) { audio_skip(1);  audio_play(); }
		if (prev_now && !prev_prev) { audio_skip(-1); audio_play(); }
		volup_prev = volup_now;
		voldn_prev = voldn_now;
		next_prev  = next_now;
		prev_prev  = prev_now;

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
					hp_apply_level(vol_level);
				} else {
					codec_headphone_mute(true);
					codec_speaker_volume(vol_r46[vol_level]);
					codec_speaker_mute(false);
				}
			}
		}

		/* pb_leds: volume bar (≈1.2 s after a vol button), else live audio VU
		 * while playing, else off. Bottom→top, partial segment dims. */
		bool playing = audio_is_playing();
		int fill;
		if (meter_ticks > 0) {
			meter_ticks--;
			fill = vol_level * NUM_PB_LEDS * PWM_TOP / 7;   /* 0..4·TOP */
		} else if (playing) {
			/* Baked level (0..255) for the current block → target fill, with a
			 * light one-pole for smooth motion. */
			int lvl    = (int)audio_vu_level();
			int target = lvl * (NUM_PB_LEDS * PWM_TOP) / VU_REF;
			if (target > NUM_PB_LEDS * PWM_TOP) target = NUM_PB_LEDS * PWM_TOP;
			vu_disp += (target - vu_disp) / 2;
			fill = vu_disp;
		} else {
			vu_disp = 0;
			fill = 0;
		}
		for (int s = 0; s < NUM_PB_LEDS; s++) {
			int b = fill - s * PWM_TOP;
			if (b < 0)        b = 0;
			if (b > PWM_TOP)  b = PWM_TOP;
			pwm1_set_duty(NUM_PB_LEDS - 1 - s, (uint16_t)b);
		}

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
