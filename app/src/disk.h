#pragma once

#include <stdint.h>
#include <stdbool.h>

/*
 * SP1F disk format — 8-channel IMA-ADPCM at 48 kHz (4 stereo stems).
 *
 * Block 0        : disk_header_t (512 bytes)
 * Blocks 1–8     : song catalog  (8 blocks × 16 entries = up to 128 songs)
 * Block 9+       : audio data, allocated sequentially
 *
 * Audio block layout (512 bytes, 8 channels, 128 samples/channel):
 *   bytes   0– 63  channel 0 (stem 0 L)  — 128 nibbles, low nibble first
 *   bytes  64–127  channel 1 (stem 0 R)
 *   bytes 128–191  channel 2 (stem 1 L)
 *   bytes 192–255  channel 3 (stem 1 R)
 *   bytes 256–319  channel 4 (stem 2 L)
 *   bytes 320–383  channel 5 (stem 2 R)
 *   bytes 384–447  channel 6 (stem 3 L)
 *   bytes 448–511  channel 7 (stem 3 R)
 *
 * ADPCM state (predictor + step_index) carries over between blocks.
 * Initial state at song start: predictor=0, step_index=0 for all 8 channels.
 *
 * Songs play in catalog order; deleted entries (name[0]=0) are skipped.
 */

#define DISK_MAGIC              0x53503146u  /* "SP1F" little-endian */
#define DISK_VERSION            1u

#define DISK_HEADER_BLOCK       0u
#define DISK_CATALOG_START      1u           /* first catalog block */
#define DISK_CATALOG_BLOCKS     8u           /* 8 catalog blocks */
#define DISK_SONGS_PER_BLOCK    16u          /* 512 / sizeof(disk_song_entry_t) */
#define DISK_MAX_SONGS          128u         /* DISK_CATALOG_BLOCKS × DISK_SONGS_PER_BLOCK */
#define DISK_DATA_START_BLOCK   9u           /* first audio data block */

#define DISK_ENTRY_FREE_MARKER  '\0'         /* name[0]=0 → free/deleted */

/* One song entry — 32 bytes. */
typedef struct {
    char     name[24];       /* null-terminated; name[0]=0 → free slot */
    uint32_t block_start;    /* first 512-byte audio block */
    uint32_t block_count;    /* number of audio blocks */
} disk_song_entry_t;

/* Master header — exactly 512 bytes. */
typedef struct {
    uint32_t magic;           /* DISK_MAGIC */
    uint8_t  version;         /* DISK_VERSION */
    uint8_t  _pad0[3];
    uint16_t song_count;      /* entries allocated (including deleted holes) */
    uint8_t  _pad1[2];
    uint32_t next_free_block; /* first unallocated audio block */
    uint8_t  _pad2[496];
} disk_header_t;

/* ── API ────────────────────────────────────────────────────────────────────── */

bool disk_read_header(disk_header_t *hdr);
bool disk_write_header(const disk_header_t *hdr);

/* Writes a fresh header (block 0) and zeroes all catalog blocks (1–8). */
bool disk_format(void);

/* Read/write a single song entry by catalog index (0–127). */
bool disk_read_song(uint16_t idx, disk_song_entry_t *entry);
bool disk_write_song(uint16_t idx, const disk_song_entry_t *entry);

/* Find a free slot or append. Writes entry to catalog and updates hdr->song_count.
 * Returns catalog index (0–127) or 0xFFFF if catalog is full. */
uint16_t disk_add_song(disk_header_t *hdr, const disk_song_entry_t *entry);

/* Zero the catalog entry at idx (marks it deleted). */
bool disk_remove_song(uint16_t idx);
