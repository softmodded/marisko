#pragma once

#include <stdint.h>
#include <stdbool.h>

/*
 * Audio codec bring-up for the SP-1.
 *
 * Two codecs on I2C0 (SDA P1.07, SCL P1.11):
 *   CS42L42 @ 0x48 — headphone codec; also generates the I2S LRCLK (frame clock)
 *                     so it MUST be initialized for any audio to clock at all.
 *   TAS2505 @ 0x18 — mono speaker class-D amp (our output target).
 *
 * Clock chain: external 3.072 MHz oscillator enabled by P0.13 high provides
 * BCLK. CS42L42 PLLs off it and produces the 48 kHz LRCLK. The nRF I2S runs
 * as SLAVE off BCLK + LRCLK.
 *
 * Reset lines (active-low): CS42L42 = P0.15, TAS2505 = P0.09 (NFC1 reclaimed).
 */

/* Bring up the oscillator + both codecs (register scripts ported from the
 * sp1-midi drivers). Leaves both outputs MUTED. Returns true if every I2C
 * transaction succeeded. */
bool codec_init(void);

/* Speaker (TAS2505) mute control. */
bool codec_speaker_mute(bool mute);

/* Speaker volume: 0x00 = max gain, 0x7F = mute (TAS2505 P1/R46 scale). */
bool codec_speaker_volume(uint8_t vol);

/* Headphone (CS42L42) mute control. HP_CTL 0x2001: 0x0E=mute, 0x02=unmute
 * (values from the sp1-midi cs42l42 driver). The codec inits muted. */
bool codec_headphone_mute(bool mute);

/* Headphone volume: writes HP_VOL_A (0x2601) + HP_VOL_B (0x2609). The stock
 * bring-up listening level is 0x4C; larger raw = more attenuation (quieter).
 * Exact dB/step is uncalibrated — tune the table in main.c by ear. */
bool codec_headphone_volume(uint8_t vol);

/* True if headphones are plugged into the TRRS jack. Reads CS42L42
 * TSRS_PLUG_STATUS (0x130F): plugged when ((reg & 0x0C) >> 2) == 0x03
 * (logic from the sp1-midi cs42l42_codec_is_headphone_connected). Tip-sense
 * detection is enabled during codec_init (MIC_DET_CTL1 + TIPSENSE_CTL). */
bool codec_headphones_present(void);

/* Diagnostic snapshot for the USB CODEC_DIAG command. 32 bytes. */
typedef struct {
	uint8_t  init_ok;        /* 1 if codec_init() returned true */
	uint8_t  cs_devid_ab;    /* expect 0x42 */
	uint8_t  cs_devid_cd;    /* expect 0xA4 */
	uint8_t  cs_devid_e;
	uint8_t  cs_pll_lock;    /* CS42L42 0x130E PLL lock status */
	uint8_t  cs_clock_ctl;   /* 0x1007 */
	uint8_t  cs_pwr_ctl1;    /* 0x1101 */
	uint8_t  cs_hp_ctl;      /* 0x2001 */
	uint8_t  tas_p0r2;       /* TAS2505 P0/R2 (overflow/PLL flags vary) */
	uint8_t  tas_p1r46;      /* speaker driver volume */
	uint8_t  tas_p1r45;      /* speaker amp control */
	uint8_t  tas_p0r63;      /* DAC channel setup */
	uint8_t  i2c_errors;     /* count of failed I2C transactions during init */
	uint8_t  audio_running;  /* set by usb layer: I2S TX stream started */
	/* live re-reads (current state, captured when CODEC_DIAG is requested) */
	uint8_t  live_cs_pll;    /* CS42L42 PLL lock now */
	uint8_t  live_tas_p1r46; /* speaker volume now (0x00=max, 0x7F=mute) */
	uint8_t  live_tas_p1r45; /* speaker amp ctl now (0x02=on) */
	uint8_t  live_tas_p0r63; /* DAC path now (0xB0=on) */
	uint8_t  live_tas_p0r64; /* DAC vol now */
	uint8_t  live_tas_p0r65; /* DAC L vol now */
	uint8_t  live_tas_p0r25; /* clock mux status */
	int8_t   audio_i2s_cfg_ret; /* i2s_configure() return code */
	uint8_t  audio_stage;       /* how far audio_init() progressed (see audio.c) */
	uint8_t  live_cs_osc_sw;    /* CS42L42 0x1109 osc switch status (0x02 = SCLK→MCLK ok) */
	uint8_t  live_cs_hp_ctl;    /* CS42L42 0x2001 HP_CTL now (0x01 unmuted, 0x0D muted) */
	uint8_t  _pad[7];
} codec_diag_t;

/* Re-read live codec state into the diag struct (called before reporting). */
void codec_refresh_diag(void);

/* Record whether the I2S stream is running, into the diag struct. */
void codec_note_audio_running(bool running);

/* Record audio_init progress: i2s_configure return code + stage reached. */
void codec_note_audio(int cfg_ret, uint8_t stage);

void codec_get_diag(codec_diag_t *out);
