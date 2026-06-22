#pragma once

#include <stdint.h>
#include <stdbool.h>

/*
 * eMMC driver for Toshiba THGBMNG5D1LBAIL (4 GB, eMMC 5.0, 1-bit mode).
 *
 * Pin map (all P0):
 *   P0.06 = CLK   GPIO bit-bang
 *   P0.07 = DAT0  GPIO bit-bang
 *   P0.08 = CMD   GPIO bit-bang
 *   P0.14 = VCCQ  I/O supply enable (active high)
 *   P1.08 = RST   Reset (active low)
 *
 * All reads are synchronous (polling). Async path not implemented.
 */

/* Initialize eMMC: power, reset, CMD0-16, cache enable.
 * Returns true on success. Must be called before any read. */
bool emmc_init(void);

/* Read a single 512-byte block at block_addr into buf (must be 4-byte aligned). */
bool emmc_read_block(uint32_t block_addr, uint8_t *buf);

/* Write a single 512-byte block at block_addr from buf (CMD24). */
bool emmc_write_block(uint32_t block_addr, const uint8_t *buf);

/*
 * Streaming CMD25 multi-block write — amortises NAND page program overhead.
 * begin → write × N → end.  Must not interleave with other eMMC calls.
 */
bool emmc_write_multi_begin(uint32_t block_addr, uint32_t num_blocks);
bool emmc_write_multi_block(const uint8_t *buf);  /* 512 bytes */
bool emmc_write_multi_end(void);
bool emmc_write_multi_active(void);  /* true if CMD25 session is open */
uint32_t emmc_mb_count(void);        /* blocks written in current/last CMD25 session */
uint8_t  emmc_mb_fail(void);         /* 0=none 1=resp-reject 2=busy-timeout */
uint32_t emmc_busy_us(void);         /* µs in busy-wait of last written block */

/* Flush eMMC write cache to NAND (call after upload before power-off). */
bool emmc_cache_flush(void);

/* Read num_blocks consecutive 512-byte blocks starting at block_addr.
 * buf must hold num_blocks * 512 bytes and be 4-byte aligned. */
bool emmc_read_blocks(uint32_t block_addr, uint8_t *buf, uint32_t num_blocks);

/* Total capacity in 512-byte blocks (valid after emmc_init succeeds). */
uint32_t emmc_capacity_blocks(void);

/* Count of read-CRC16 mismatches seen by emmc_read_blocks (corrupt reads). */
uint32_t emmc_crc_errors(void);

/* Write cache size in KiB from EXT_CSD[168..171]; 0 = no cache on this card. */
uint32_t emmc_cache_size_kb(void);

/* True if CMD6 CACHE_EN succeeded during emmc_init. */
bool emmc_cache_enabled(void);

/* Pointer to the cached 512-byte EXT_CSD read during emmc_init (for diagnostics). */
const uint8_t *emmc_ext_csd(void);

/*
 * Which init step failed (valid when emmc_init returns false).
 * 1=CMD0  2=CMD1(poll)  3=CMD2  4=CMD3  5=CMD9  6=CMD7
 * 7=CMD8/EXT_CSD  8=CMD16  9=not-in-TRANSFER-state
 */
uint8_t emmc_fail_step(void);
