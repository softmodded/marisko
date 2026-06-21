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

	/* Block-0 read check: all 4 leds flash twice = success, led[0] fast blink = fail. */
	{
		static uint8_t block0[512];
		if (emmc_read_block(0, block0)) {
			for (int flash = 0; flash < 2; flash++) {
				for (int i = 0; i < NUM_PB_LEDS; i++) set_pb_on(i);
				delay_ms(300);
				all_pb_off();
				delay_ms(200);
				feed_wdt();
			}
		} else {
			for (int i = 0; i < 8; i++) {
				set_pb_on(0);
				delay_ms(100);
				all_pb_off();
				delay_ms(100);
			}
			feed_wdt();
		}
	}

	/* Cache diagnostic: 4 LEDs = cache+CMD6, 2 = cache only, 0 = no cache. */
	{
		feed_wdt();
		delay_ms(500);
		int leds = 0;
		if (emmc_cache_size_kb() > 0) leds = emmc_cache_enabled() ? 4 : 2;
		for (int i = 0; i < leds; i++) set_trk_on(i);
		delay_ms(1000);
		all_trk_off();
		feed_wdt();
	}

	/* Audio codec bring-up. pb_led[3] solid = init clean; track[3] blink = I2C error.
	 * Detailed register state available over USB via CODEC_DIAG (0x0B). */
	{
		feed_wdt();
		bool codec_ok = codec_init();
		feed_wdt();
		if (codec_ok) {
			set_pb_on(3);
			delay_ms(600);
			all_pb_off();
		} else {
			for (int i = 0; i < 4; i++) {
				set_trk_on(3);
				delay_ms(150);
				all_trk_off();
				delay_ms(150);
			}
		}
		feed_wdt();

		/* Scan disk for first valid song BEFORE starting I2S (no concurrent DMA).
		 * Track LED 0 blink count:
		 *   1 = header ok but no songs
		 *   2 = song found → will play
		 *   3 = disk_read_header failed */
		static disk_header_t s_dh;
		static disk_song_entry_t s_se;
		uint32_t song_block_start = 0, song_block_count = 0;
		uint16_t first_song_idx = 0, total_songs = 0;
		bool song_found = false;
		{
			int diag_blinks;
			if (!disk_read_header(&s_dh)) {
				diag_blinks = 3;
			} else if (s_dh.song_count == 0) {
				diag_blinks = 1;
			} else {
				diag_blinks = 1;
				total_songs = s_dh.song_count;
				for (uint16_t i = 0; i < s_dh.song_count; i++) {
					if (disk_read_song(i, &s_se) && s_se.name[0] != '\0') {
						diag_blinks = 2;
						song_found = true;
						first_song_idx   = i;
						song_block_start = s_se.block_start;
						song_block_count  = s_se.block_count;
						break;
					}
				}
			}
			for (int f = 0; f < diag_blinks; f++) {
				set_trk_on(0); delay_ms(200);
				all_trk_off();  delay_ms(200);
				feed_wdt();
			}
		}
		feed_wdt();

		/* Stage 2: I2S bring-up. Stage 3: ADPCM playback. */
		if (codec_ok && audio_init()) {
			if (song_found) {
				audio_set_source(AUDIO_SRC_ADPCM);
				audio_set_playlist(total_songs, first_song_idx);
				audio_load_song(song_block_start, song_block_count);
				/* Start paused; play button toggles. Feed thread auto-advances
				 * to the next song at end, wrapping after the last. */
			}
		}
		feed_wdt();
	}

	/* LED bounce state */
	int pb_pos = 0;
	int pb_dir = 1;

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
	int meter_ticks = 0;   /* loops left to show the volume bar before bounce resumes */
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

		/* Volume +/- (edge-triggered) on ladder 2 (AIN1). */
		int vladder = saadc_read(2u);  /* AIN1 */
		int volup_now = (vladder >= 1600 && vladder <= 1980);
		int voldn_now = (vladder >=  600 && vladder <=  860);
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
		volup_prev = volup_now;
		voldn_prev = voldn_now;

		/* pb_leds: volume bar on change (≈1.2 s), bounce animation otherwise.
		 * Bar fills bottom→top (pb[3]=bottom) and the partial segment dims to
		 * show the in-between level (8 levels across 4 LEDs). */
		if (meter_ticks > 0) {
			meter_ticks--;
			int fill = vol_level * NUM_PB_LEDS * PWM_TOP / 7;  /* 0..4·TOP */
			for (int s = 0; s < NUM_PB_LEDS; s++) {
				int b = fill - s * PWM_TOP;
				if (b < 0)        b = 0;
				if (b > PWM_TOP)  b = PWM_TOP;
				pwm1_set_duty(NUM_PB_LEDS - 1 - s, (uint16_t)b);
			}
		} else {
			all_pb_off();
			set_pb_on(pb_pos);
			pb_pos += pb_dir;
			if (pb_pos == NUM_PB_LEDS - 1) pb_dir = -1;
			if (pb_pos == 0)               pb_dir =  1;
		}

		/* Renode mirror: expose GPIO state for emulator observation */
		*(volatile uint32_t *)0x2000FFF0 = NRF_P0->OUT;
		*(volatile uint32_t *)0x2000FFF4 = NRF_P1->OUT;

		usb_cdc_poll();

		feed_wdt();
		if (!uploading)
			k_msleep(30);   /* yield CPU to the feed thread between polls */

		if (!(NRF_P0->IN & (1u << 27)))
			enter_system_off();
	}

	return 0;
}
