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
	int next_prev  = 0;
	int prev_prev  = 0;
	int meter_ticks = 0;   /* loops left to show the volume bar before bounce resumes */

	/* Audio VU: reference for a full 4-bar bar (higher = less sensitive) and a
	 * displayed envelope with instant attack + smooth time-based decay. */
	const int VU_REF = 22000;
	int vu_disp = 0;
	codec_speaker_volume(vol_r46[vol_level]);

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
		if (volup_now && !volup_prev && vol_level < 7) {
			vol_level++;
			codec_speaker_volume(vol_r46[vol_level]);
			meter_ticks = 40;
		}
		if (voldn_now && !voldn_prev && vol_level > 0) {
			vol_level--;
			codec_speaker_volume(vol_r46[vol_level]);
			meter_ticks = 40;
		}
		/* Prev/next rocker: skip song + ensure playing. */
		if (next_now && !next_prev) { audio_skip(1);  audio_play(); }
		if (prev_now && !prev_prev) { audio_skip(-1); audio_play(); }
		volup_prev = volup_now;
		voldn_prev = voldn_now;
		next_prev  = next_now;
		prev_prev  = prev_now;

		/* pb_leds: volume bar (≈1.2 s after a vol button), else live audio VU
		 * while playing, else off. Bottom→top, partial segment dims. */
		bool playing = audio_is_playing();
		int fill;
		if (meter_ticks > 0) {
			meter_ticks--;
			fill = vol_level * NUM_PB_LEDS * PWM_TOP / 7;   /* 0..4·TOP */
		} else if (playing) {
			/* Instant attack, smooth decay (per loop) → smooth bounce, no freeze
			 * even while the feed thread is mid eMMC read. */
			int target = (int)audio_level_take();
			if (target > vu_disp) vu_disp = target;
			else                  vu_disp -= vu_disp / 6;
			fill = (int)((int64_t)vu_disp * (NUM_PB_LEDS * PWM_TOP) / VU_REF);
			if (fill > NUM_PB_LEDS * PWM_TOP) fill = NUM_PB_LEDS * PWM_TOP;
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
