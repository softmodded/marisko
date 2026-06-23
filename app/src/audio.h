#pragma once

#include <stdint.h>
#include <stdbool.h>

/*
 * I2S audio output for the SP-1.
 *
 * nRF I2S0 runs as SLAVE: BCLK (3.072 MHz osc) + LRCLK (from CS42L42) are
 * external. 24-bit SWIDTH, stereo, 48 kHz. 32-bit DMA words; sample in [31:8].
 * A dedicated feed thread keeps the TX double-buffer primed.
 *
 * codec_init() must succeed (CS42L42 PLL locked) before audio_init().
 */

typedef enum {
	AUDIO_SRC_SILENCE = 0,
	AUDIO_SRC_TONE,        /* built-in 440 Hz sine — bring-up test */
	AUDIO_SRC_ADPCM,       /* IMA-ADPCM from eMMC (use audio_load_song first) */
} audio_source_t;

/* Configure + start the I2S stream and spawn the feed thread.
 * Unmutes the speaker. Returns false if the I2S device isn't ready. */
bool audio_init(void);

/* Select what the feed thread emits. */
void audio_set_source(audio_source_t src);

/* True once the I2S TX stream has actually started (START trigger succeeded). */
bool audio_running(void);

/* ── ADPCM playback ─────────────────────────────────────────────────────────── */

/* Tell the feed thread the catalog size + current index so it can auto-advance
 * to the next song at end-of-song (wrapping after the last). Call before
 * audio_load_song(). */
void audio_set_playlist(uint16_t song_count, uint16_t current_idx);

/* Enable baked per-block VU levels (disk v2). The level array (one decimated
 * byte per LVL_DECIM audio blocks) follows each song's audio at
 * block_start + block_count; the feed thread loads it into RAM once at song
 * start. Disable for v1 discs → falls back to on-device peak. Call before
 * audio_load_song(). */
void audio_set_levels_enabled(bool enabled);

/* Set the song to play (block_start + block_count from the disk catalog).
 * Resets ADPCM state to silence; call audio_play() to start. */
void audio_load_song(uint32_t block_start, uint32_t block_count);

/* Live per-stem mix gain from the 4 faders (0..256, 256 = unity, 0 = silent).
 * Index = stem (0..3). The feed thread applies these in the mix; muted stems
 * still advance their ADPCM decoder so they never desync. Safe from main. */
void audio_set_stem_gains(const uint16_t g[4]);

/* Play / pause / toggle. Safe to call from the main thread at any time. */
void audio_play(void);
void audio_pause(void);
void audio_toggle(void);
bool audio_is_playing(void);

/* Skip to next (dir>0) or previous (dir<0) song. Processed by the feed thread;
 * the song change reads the catalog (eMMC) there to avoid a bus race. */
void audio_skip(int dir);

/* Overall VU level (0..255) for the pb-LED meter at block `blk` — the four
 * baked stem levels summed and scaled by the live fader gains (disk v3). Falls
 * back to on-device peak-hold on discs without baked levels. The caller passes
 * a smooth real-time block estimate (not audio_cur_block(), which freezes during
 * eMMC reads → choppy meters) and applies light smoothing. */
uint32_t audio_vu_level_at(uint32_t blk);

/* Baked per-stem VU level (0..255) at block `blk`, scaled by the stem's live
 * fader gain (disk v3). Index = stem 0..3. For the track LEDs. 0 if no levels. */
uint32_t audio_stem_level_at(uint32_t blk, int stem);

/* Diagnostic: current playback position (relative block index into the song). */
uint32_t audio_cur_block(void);

/* Diagnostic: µs per block of the last eMMC refill read (budget is 2670 µs). */
uint32_t audio_last_read_us(void);

/* Feed-thread health snapshot (USB AUDIO_DIAG). recoveries/write_fails > 0 means
 * the I2S TX queue underran; max_read_us > 2670 means an eMMC read missed the
 * realtime budget. blocks_fed should climb steadily during playback. */
typedef struct {
	uint32_t recoveries;
	uint32_t write_fails;
	uint32_t max_read_us;
	uint32_t last_read_us;
	uint32_t cur_block;
	uint32_t blocks_fed;
	uint32_t crc_errors;   /* corrupt eMMC reads caught by CRC16 (mirror of emmc_crc_errors) */
} audio_diag_t;
void audio_get_diag(audio_diag_t *d);
