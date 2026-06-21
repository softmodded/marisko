#include "disk.h"
#include "emmc.h"
#include <string.h>

_Static_assert(sizeof(disk_song_entry_t) == 32, "disk_song_entry_t must be 32 bytes");
_Static_assert(sizeof(disk_header_t) == 512, "disk_header_t must be 512 bytes");

static uint8_t s_cat_buf[512];

bool disk_read_header(disk_header_t *hdr)
{
    if (!hdr) return false;
    if (!emmc_read_block(DISK_HEADER_BLOCK, (uint8_t *)hdr)) return false;
    return hdr->magic == DISK_MAGIC && hdr->version == DISK_VERSION;
}

bool disk_write_header(const disk_header_t *hdr)
{
    if (!hdr) return false;
    return emmc_write_block(DISK_HEADER_BLOCK, (const uint8_t *)hdr);
}

bool disk_format(void)
{
    static disk_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic            = DISK_MAGIC;
    hdr.version          = DISK_VERSION;
    hdr.song_count       = 0;
    hdr.next_free_block  = DISK_DATA_START_BLOCK;
    if (!disk_write_header(&hdr)) return false;

    memset(s_cat_buf, 0, sizeof(s_cat_buf));
    for (uint8_t b = 0; b < DISK_CATALOG_BLOCKS; b++) {
        if (!emmc_write_block(DISK_CATALOG_START + b, s_cat_buf)) return false;
    }
    return true;
}

static uint32_t cat_block(uint16_t idx)
{
    return DISK_CATALOG_START + idx / DISK_SONGS_PER_BLOCK;
}

static uint32_t cat_offset(uint16_t idx)
{
    return (idx % DISK_SONGS_PER_BLOCK) * (uint32_t)sizeof(disk_song_entry_t);
}

bool disk_read_song(uint16_t idx, disk_song_entry_t *entry)
{
    if (!entry || idx >= DISK_MAX_SONGS) return false;
    if (!emmc_read_block(cat_block(idx), s_cat_buf)) return false;
    memcpy(entry, s_cat_buf + cat_offset(idx), sizeof(disk_song_entry_t));
    return true;
}

bool disk_write_song(uint16_t idx, const disk_song_entry_t *entry)
{
    if (!entry || idx >= DISK_MAX_SONGS) return false;
    if (!emmc_read_block(cat_block(idx), s_cat_buf)) return false;
    memcpy(s_cat_buf + cat_offset(idx), entry, sizeof(disk_song_entry_t));
    return emmc_write_block(cat_block(idx), s_cat_buf);
}

uint16_t disk_add_song(disk_header_t *hdr, const disk_song_entry_t *entry)
{
    if (!hdr || !entry || entry->name[0] == DISK_ENTRY_FREE_MARKER) return 0xFFFFu;

    /* Scan allocated range for a deleted hole to reuse */
    for (uint16_t i = 0; i < hdr->song_count; i++) {
        disk_song_entry_t e;
        if (!disk_read_song(i, &e)) return 0xFFFFu;
        if (e.name[0] == DISK_ENTRY_FREE_MARKER) {
            if (!disk_write_song(i, entry)) return 0xFFFFu;
            return i;
        }
    }

    /* Append */
    if (hdr->song_count >= DISK_MAX_SONGS) return 0xFFFFu;
    uint16_t idx = hdr->song_count;
    if (!disk_write_song(idx, entry)) return 0xFFFFu;
    hdr->song_count++;
    return idx;
}

bool disk_remove_song(uint16_t idx)
{
    disk_song_entry_t empty;
    memset(&empty, 0, sizeof(empty));
    return disk_write_song(idx, &empty);
}
