#include "usb.h"
#include "disk.h"
#include "emmc.h"
#include "codec.h"
#include "audio.h"
#include "saadc.h"
#include "util.h"

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/ring_buffer.h>

static const struct device *cdc_dev;

RING_BUF_DECLARE(g_rx_rb, 65536);

/* ── Low-level I/O ───────────────────────────────────────────────────────────── */

static void uart_cb(const struct device *dev, void *user_data)
{
	(void)user_data;
	while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
		if (uart_irq_rx_ready(dev)) {
			/* Pull straight into the ring's contiguous claim region — avoids the
			 * bounce buffer + a second copy, and drains the whole CDC FIFO in
			 * one go for higher RX throughput. */
			uint8_t *dst;
			uint32_t space = ring_buf_put_claim(&g_rx_rb, &dst, 4096);
			if (space == 0) {
				/* Ring full: drain FIFO to a scratch buf to keep USB flowing. */
				uint8_t tmp[64];
				int n = uart_fifo_read(dev, tmp, sizeof(tmp));
				(void)n;
				ring_buf_put_finish(&g_rx_rb, 0);
				continue;
			}
			int n = uart_fifo_read(dev, dst, space);
			ring_buf_put_finish(&g_rx_rb, n > 0 ? (uint32_t)n : 0);
		}
	}
}

static bool usb_read_bytes(uint8_t *buf, uint32_t len, uint32_t timeout_loops)
{
	uint32_t got = 0;
	while (got < len) {
		uint32_t n = ring_buf_get(&g_rx_rb, buf + got, len - got);
		got += n;
		if (got < len) {
			if (timeout_loops-- == 0) return false;
			/* Sleep (don't spin) so the lower-priority USBD thread gets CPU to
			 * service bulk-OUT and refill the ring — spinning here starves it
			 * and throttles upload throughput. 1 tick ≈ 30 µs. */
			k_sleep(K_TICKS(1));
			feed_wdt();
		}
	}
	return true;
}

static void usb_write_bytes(const uint8_t *buf, uint32_t len)
{
	for (uint32_t i = 0; i < len; i++)
		uart_poll_out(cdc_dev, buf[i]);
}

static void send_ok(const uint8_t *payload, uint32_t plen)
{
	uint8_t hdr[5];
	hdr[0] = USB_STATUS_OK;
	hdr[1] = (uint8_t)(plen & 0xFFu);
	hdr[2] = (uint8_t)((plen >> 8) & 0xFFu);
	hdr[3] = (uint8_t)((plen >> 16) & 0xFFu);
	hdr[4] = (uint8_t)((plen >> 24) & 0xFFu);
	usb_write_bytes(hdr, 5);
	if (payload && plen > 0)
		usb_write_bytes(payload, plen);
}

static void send_ok_stream(uint32_t total_len)
{
	uint8_t hdr[5];
	hdr[0] = USB_STATUS_OK;
	hdr[1] = (uint8_t)(total_len & 0xFFu);
	hdr[2] = (uint8_t)((total_len >> 8) & 0xFFu);
	hdr[3] = (uint8_t)((total_len >> 16) & 0xFFu);
	hdr[4] = (uint8_t)((total_len >> 24) & 0xFFu);
	usb_write_bytes(hdr, 5);
}

static void send_err(void)
{
	uint8_t hdr[5] = { USB_STATUS_ERR, 0, 0, 0, 0 };
	usb_write_bytes(hdr, 5);
}

/* ── Upload state ────────────────────────────────────────────────────────────── */

static uint8_t s_block_buf[512];

typedef struct {
	bool     active;
	bool     multi_begun;  /* CMD25 session is open */
	uint16_t song_idx;
	uint32_t block_start;
	uint32_t blocks_expected;
	uint32_t blocks_written;
	uint32_t session_left;   /* blocks remaining in the current CMD25 chunk */
} upload_t;

static upload_t    g_upload;
static disk_header_t g_hdr;
static bool        g_hdr_cached;
static uint8_t     g_ul_fail;       /* 0=none 1=usb-read-timeout 2=write-fail */
static uint32_t    g_ul_fail_block; /* block index where it failed */

static bool ensure_hdr(void)
{
	if (g_hdr_cached) return true;
	if (!disk_read_header(&g_hdr)) return false;
	g_hdr_cached = true;
	return true;
}

/* ── Command handlers ────────────────────────────────────────────────────────── */

static void handle_ping(void) { send_ok(NULL, 0); }

static void handle_disk_info(void)
{
	if (!emmc_read_block(0, s_block_buf)) { send_err(); return; }
	send_ok(s_block_buf, 512);
}

static void handle_extcsd_dump(void)
{
	send_ok(emmc_ext_csd(), 512);
}

/* 0x0C READ_BLOCK: block_addr[4 LE] → data[512]. Pauses audio to avoid eMMC
 * bus contention with the feed thread. Diagnostic only. */
static void handle_read_block(const uint8_t *payload, uint32_t plen)
{
	if (plen < 4) { send_err(); return; }
	audio_pause();
	k_sleep(K_MSEC(10));   /* let any in-flight feed-thread read finish */
	uint32_t addr = (uint32_t)payload[0]        | ((uint32_t)payload[1] << 8) |
	                ((uint32_t)payload[2] << 16) | ((uint32_t)payload[3] << 24);
	if (!emmc_read_block(addr, s_block_buf)) { send_err(); return; }
	send_ok(s_block_buf, 512);
}

/* 0x0D WRITE_PROBE: block_addr[4 LE] → writes a test pattern, reads it back,
 * returns 1 byte: 0=write-fail 1=readback-fail 2=mismatch 3=ok.
 * DESTRUCTIVE to that block — diagnostic for mapping the writable region. */
static uint8_t s_probe_rb[512];
static void handle_write_probe(const uint8_t *payload, uint32_t plen)
{
	if (plen < 4) { send_err(); return; }
	audio_pause();
	k_sleep(K_MSEC(10));
	uint32_t addr = (uint32_t)payload[0]        | ((uint32_t)payload[1] << 8) |
	                ((uint32_t)payload[2] << 16) | ((uint32_t)payload[3] << 24);
	for (int i = 0; i < 512; i++)
		s_block_buf[i] = (uint8_t)(i * 7u + addr + 0x5Au);
	uint8_t result;
	if (!emmc_write_block(addr, s_block_buf)) {
		result = 0;
	} else if (!emmc_read_block(addr, s_probe_rb)) {
		result = 1;
	} else {
		result = 3;
		for (int i = 0; i < 512; i++)
			if (s_probe_rb[i] != s_block_buf[i]) { result = 2; break; }
	}
	send_ok(&result, 1);
}

/* 0x0E WRITE_STRESS: count[4 LE] → writes `count` test blocks starting at
 * block 9 via CMD24 (no USB per block), returns first-fail block index [4 LE]
 * (0xFFFFFFFF = all ok). Maps cumulative-write failures. DESTRUCTIVE. */
static void handle_write_stress(const uint8_t *payload, uint32_t plen)
{
	if (plen < 4) { send_err(); return; }
	audio_pause();
	k_sleep(K_MSEC(10));
	uint32_t count = (uint32_t)payload[0]        | ((uint32_t)payload[1] << 8) |
	                 ((uint32_t)payload[2] << 16) | ((uint32_t)payload[3] << 24);
	uint32_t first_fail = 0xFFFFFFFFu;
	for (uint32_t i = 0; i < count; i++) {
		for (int b = 0; b < 512; b++)
			s_block_buf[b] = (uint8_t)(b + i);
		feed_wdt();
		if (!emmc_write_block(9u + i, s_block_buf)) { first_fail = i; break; }
	}
	uint8_t r[4] = { (uint8_t)first_fail, (uint8_t)(first_fail >> 8),
	                 (uint8_t)(first_fail >> 16), (uint8_t)(first_fail >> 24) };
	send_ok(r, 4);
}

static void handle_codec_diag(void)
{
	/* Returns the snapshot captured at boot — no live I2C here, because a TWIM
	 * read can block K_FOREVER and trip the watchdog from this context. */
	codec_refresh_diag();
	codec_diag_t diag;
	codec_get_diag(&diag);
	/* Stash playback position into the pad bytes (offset 23..26) for diagnostics. */
	uint32_t cb = audio_cur_block();
	((uint8_t *)&diag)[23] = (uint8_t)(cb);
	((uint8_t *)&diag)[24] = (uint8_t)(cb >> 8);
	((uint8_t *)&diag)[25] = (uint8_t)(cb >> 16);
	((uint8_t *)&diag)[26] = (uint8_t)(cb >> 24);
	uint32_t ru = audio_last_read_us();
	((uint8_t *)&diag)[27] = (uint8_t)(ru);
	((uint8_t *)&diag)[28] = (uint8_t)(ru >> 8);
	/* Live AIN0 (button ladder 1: play + 4 track buttons) into bytes 29..30. */
	int ain0 = saadc_read(1u);  /* PSELP=1 → AIN0 (ladder 1: play + tracks) */
	if (ain0 < 0) ain0 = 0;
	((uint8_t *)&diag)[29] = (uint8_t)(ain0);
	((uint8_t *)&diag)[30] = (uint8_t)((uint32_t)ain0 >> 8);
	/* Multiblock write diag: cur_block(23..26) reused for mb_count; read_us hi
	 * byte (28) reused for fail reason (valid when not playing). */
	/* Upload-fail diag: fail block (23..26), fail reason (28: 1=usb 2=write). */
	((uint8_t *)&diag)[23] = (uint8_t)(g_ul_fail_block);
	((uint8_t *)&diag)[24] = (uint8_t)(g_ul_fail_block >> 8);
	((uint8_t *)&diag)[25] = (uint8_t)(g_ul_fail_block >> 16);
	((uint8_t *)&diag)[26] = (uint8_t)(g_ul_fail_block >> 24);
	((uint8_t *)&diag)[28] = g_ul_fail;
	/* Last-block busy-wait µs into bytes 29..30 (overwrites AIN0 below if set). */
	uint32_t bw = emmc_busy_us();
	((uint8_t *)&diag)[29] = (uint8_t)(bw);
	((uint8_t *)&diag)[30] = (uint8_t)(bw >> 8);
	send_ok((const uint8_t *)&diag, sizeof(diag));
}

static void handle_disk_format(void)
{
	g_hdr_cached = false;
	if (g_upload.multi_begun) {
		emmc_write_multi_end();
		g_upload.multi_begun = false;
	}
	g_upload.active = false;
	if (!disk_format()) { send_err(); return; }
	g_hdr_cached = false;
	send_ok(NULL, 0);
}

/* 0x04 SONG_BEGIN: name[24] + nblocks[4] = 28 bytes → song_idx[2 LE] */
static void handle_song_begin(const uint8_t *payload, uint32_t plen)
{
	if (plen < 28) { send_err(); return; }
	if (!ensure_hdr()) { send_err(); return; }
	if (g_upload.active) { send_err(); return; }

	/* Pause audio so feed thread stops issuing CMD17 reads during disk ops */
	audio_pause();

	disk_song_entry_t entry = {0};
	for (int i = 0; i < 24; i++) entry.name[i] = (char)payload[i];
	entry.name[23]    = '\0';
	entry.block_start = g_hdr.next_free_block;
	entry.block_count = (uint32_t)payload[24]        | ((uint32_t)payload[25] << 8) |
	                    ((uint32_t)payload[26] << 16) | ((uint32_t)payload[27] << 24);

	uint16_t idx = disk_add_song(&g_hdr, &entry);
	if (idx == 0xFFFFu) { send_err(); return; }

	if (!disk_write_header(&g_hdr)) { send_err(); return; }
	g_hdr_cached = true;

	g_upload.active          = true;
	g_upload.multi_begun     = false;
	g_upload.song_idx        = idx;
	g_upload.block_start     = g_hdr.next_free_block;
	g_upload.blocks_expected = entry.block_count;
	g_upload.blocks_written  = 0;

	uint8_t resp[2] = { (uint8_t)(idx & 0xFFu), (uint8_t)(idx >> 8) };
	send_ok(resp, 2);
}

/* 0x09 SONG_MULTIBLOCK: payload = count[2 LE]; followed by count×512 raw bytes.
 * CMD25 streaming (SPIM3 16 MHz burst per block) with the write cache on — the
 * fast path. Session opened lazily on the first batch, kept open across batches,
 * closed at SONG_COMMIT. */
static void handle_song_multiblock(const uint8_t *count_payload)
{
	uint16_t count = (uint16_t)count_payload[0] | ((uint16_t)count_payload[1] << 8);
	if (!g_upload.active || count == 0) { send_err(); return; }
	if ((uint32_t)g_upload.blocks_written + count > g_upload.blocks_expected) {
		send_err(); return;
	}

	if (!g_upload.multi_begun) {
		if (!emmc_write_multi_begin(g_upload.block_start, g_upload.blocks_expected)) {
			g_ul_fail = 2; g_ul_fail_block = g_upload.blocks_written; send_err(); return;
		}
		g_upload.multi_begun = true;
	}

	bool ok = true;
	for (uint16_t i = 0; i < count; i++) {
		if (!usb_read_bytes(s_block_buf, 512, 1000000)) {
			g_ul_fail = 1; g_ul_fail_block = g_upload.blocks_written; ok = false; break;
		}
		feed_wdt();
		if (!emmc_write_multi_block(s_block_buf)) {
			g_ul_fail = 2; g_ul_fail_block = g_upload.blocks_written; ok = false; break;
		}
		g_upload.blocks_written++;
	}

	if (ok) {
		send_ok(NULL, 0);
	} else {
		emmc_write_multi_end();
		g_upload.multi_begun = false;
		g_upload.active = false;
		send_err();
		k_sleep(K_MSEC(50));
		ring_buf_reset(&g_rx_rb);
	}
}

/* 0x05 SONG_BLOCK: block_data[512] */
static void handle_song_block(const uint8_t *payload, uint32_t plen)
{
	if (!g_upload.active || plen < 512) { send_err(); return; }
	if (g_upload.blocks_written >= g_upload.blocks_expected) { send_err(); return; }

	uint32_t block_addr = g_upload.block_start + g_upload.blocks_written;

	feed_wdt();
	if (!emmc_write_block(block_addr, payload)) { send_err(); return; }

	g_upload.blocks_written++;
	send_ok(NULL, 0);
}

/* 0x06 SONG_COMMIT */
static void handle_song_commit(void)
{
	if (!g_upload.active) { send_err(); return; }

	/* Close CMD25 session */
	if (g_upload.multi_begun) {
		emmc_write_multi_end();
		g_upload.multi_begun = false;
	}

	/* Update catalog entry with actual block_count */
	disk_song_entry_t entry;
	if (!disk_read_song(g_upload.song_idx, &entry)) { send_err(); return; }
	entry.block_count = g_upload.blocks_written;
	if (!disk_write_song(g_upload.song_idx, &entry)) { send_err(); return; }

	/* Advance next_free_block */
	g_hdr.next_free_block = g_upload.block_start + g_upload.blocks_written;
	g_upload.active = false;

	if (!disk_write_header(&g_hdr)) { send_err(); return; }
	g_hdr_cached = true;
	send_ok(NULL, 0);
}

/* 0x07 SONG_REMOVE: song_idx[2 LE] */
static void handle_song_remove(const uint8_t *payload, uint32_t plen)
{
	if (plen < 2) { send_err(); return; }
	uint16_t idx = (uint16_t)payload[0] | ((uint16_t)payload[1] << 8);
	if (!disk_remove_song(idx)) { send_err(); return; }
	send_ok(NULL, 0);
}

/* 0x08 CATALOG_READ: stream all 8 catalog blocks (4096 bytes total) */
static void handle_catalog_read(void)
{
	send_ok_stream(DISK_CATALOG_BLOCKS * 512u);
	for (uint8_t b = 0; b < DISK_CATALOG_BLOCKS; b++) {
		if (!emmc_read_block(DISK_CATALOG_START + b, s_block_buf)) {
			/* Already sent header — fill remainder with zeros on error */
			uint8_t zeros[512] = {0};
			usb_write_bytes(zeros, 512);
		} else {
			usb_write_bytes(s_block_buf, 512);
		}
	}
}

/* ── Command dispatch ────────────────────────────────────────────────────────── */

static void dispatch(uint8_t cmd, const uint8_t *payload, uint32_t plen)
{
	switch (cmd) {
	case USB_CMD_PING:         handle_ping();                          break;
	case USB_CMD_DISK_INFO:    handle_disk_info();                     break;
	case USB_CMD_DISK_FORMAT:  handle_disk_format();                   break;
	case USB_CMD_SONG_BEGIN:   handle_song_begin(payload, plen);       break;
	case USB_CMD_SONG_BLOCK:   handle_song_block(payload, plen);       break;
	case USB_CMD_SONG_COMMIT:  handle_song_commit();                   break;
	case USB_CMD_SONG_REMOVE:  handle_song_remove(payload, plen);      break;
	case USB_CMD_CATALOG_READ:    handle_catalog_read();                break;
	case USB_CMD_SONG_MULTIBLOCK: handle_song_multiblock(s_block_buf); break;
	case USB_CMD_EXTCSD_DUMP:     handle_extcsd_dump();                 break;
	case USB_CMD_CODEC_DIAG:      handle_codec_diag();                  break;
	case USB_CMD_READ_BLOCK:      handle_read_block(payload, plen);     break;
	case USB_CMD_WRITE_PROBE:     handle_write_probe(payload, plen);    break;
	case USB_CMD_WRITE_STRESS:    handle_write_stress(payload, plen);   break;
	default:                      send_err();                          break;
	}
}

/* ── Public API ──────────────────────────────────────────────────────────────── */

bool usb_cdc_init(void)
{
	cdc_dev = DEVICE_DT_GET_ONE(zephyr_cdc_acm_uart);
	if (!device_is_ready(cdc_dev)) return false;
	uart_irq_callback_set(cdc_dev, uart_cb);
	uart_irq_rx_enable(cdc_dev);
	return true;
}

bool usb_upload_active(void)
{
	return g_upload.active;
}

bool usb_cdc_connected(void)
{
	if (!cdc_dev) return false;
	uint32_t dtr = 0;
	uart_line_ctrl_get(cdc_dev, UART_LINE_CTRL_DTR, &dtr);
	return dtr != 0;
}

void usb_cdc_poll(void)
{
	if (!cdc_dev) return;

	if (ring_buf_size_get(&g_rx_rb) < 5) return;

	uint8_t hdr[5];
	if (!usb_read_bytes(hdr, 5, 0)) return;

	uint8_t  cmd  = hdr[0];
	uint32_t plen = (uint32_t)hdr[1]        | ((uint32_t)hdr[2] << 8) |
	                ((uint32_t)hdr[3] << 16) | ((uint32_t)hdr[4] << 24);

	if (plen > sizeof(s_block_buf)) {
		uint8_t drain[64];
		uint32_t remaining = plen;
		while (remaining > 0) {
			uint32_t chunk = remaining < sizeof(drain) ? remaining : sizeof(drain);
			usb_read_bytes(drain, chunk, 100000);
			remaining -= chunk;
		}
		send_err();
		return;
	}

	if (plen > 0 && !usb_read_bytes(s_block_buf, plen, 1000000)) {
		send_err();
		return;
	}

	dispatch(cmd, s_block_buf, plen);
}
