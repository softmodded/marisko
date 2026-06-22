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

/* Play / pause / toggle. Safe to call from the main thread at any time. */
void audio_play(void);
void audio_pause(void);
void audio_toggle(void);
bool audio_is_playing(void);

/* Skip to next (dir>0) or previous (dir<0) song. Processed by the feed thread;
 * the song change reads the catalog (eMMC) there to avoid a bus race. */
void audio_skip(int dir);

/* Current VU level (0..255) for the LED meter, indexed by playback position.
 * With baked levels (disk v2) this reads the precomputed level for the block
 * being decoded from RAM (loaded once at song start) — no per-frame eMMC. Falls
 * back to on-device peak-hold on v1 discs. Caller applies light smoothing. */
uint32_t audio_vu_level(void);

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
