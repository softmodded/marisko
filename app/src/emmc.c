#include "emmc.h"
#include "util.h"
#include <soc.h>
#include <stddef.h>
#include <string.h>

/* ── Pin constants ─────────────────────────────────────────────────────────── */

#define PIN_CLK  6u   /* P0.06 */
#define PIN_DAT0 7u   /* P0.07 */
#define PIN_CMD  8u   /* P0.08 */
#define PIN_VCCQ 14u  /* P0.14 */
#define PIN_RST  8u   /* P1.08 */

#define CLK_MASK  (1u << PIN_CLK)
#define CMD_MASK  (1u << PIN_CMD)
#define DAT0_MASK (1u << PIN_DAT0)

/* High-drive output (H0H1) for CLK and CMD to support 32 MHz */
#define CNF_OUTPUT_HD \
	((GPIO_PIN_CNF_DIR_Output        << GPIO_PIN_CNF_DIR_Pos)   | \
	 (GPIO_PIN_CNF_INPUT_Disconnect  << GPIO_PIN_CNF_INPUT_Pos) | \
	 (GPIO_PIN_CNF_PULL_Disabled     << GPIO_PIN_CNF_PULL_Pos)  | \
	 (GPIO_PIN_CNF_DRIVE_H0H1        << GPIO_PIN_CNF_DRIVE_Pos) | \
	 (GPIO_PIN_CNF_SENSE_Disabled    << GPIO_PIN_CNF_SENSE_Pos))

/* Input with pull-up (DAT0 and CMD response) */
#define CNF_INPUT_PULLUP \
	((GPIO_PIN_CNF_DIR_Input         << GPIO_PIN_CNF_DIR_Pos)   | \
	 (GPIO_PIN_CNF_INPUT_Connect     << GPIO_PIN_CNF_INPUT_Pos) | \
	 (GPIO_PIN_CNF_PULL_Pullup       << GPIO_PIN_CNF_PULL_Pos)  | \
	 (GPIO_PIN_CNF_DRIVE_S0S1        << GPIO_PIN_CNF_DRIVE_Pos) | \
	 (GPIO_PIN_CNF_SENSE_Disabled    << GPIO_PIN_CNF_SENSE_Pos))

/* ── Module state ──────────────────────────────────────────────────────────── */

static uint16_t g_rca;
static uint32_t g_capacity_blocks;
static uint32_t g_cache_size_kb;
static bool     g_initialized;
static bool     g_cache_enabled;
static bool     g_rel_write_off;
static uint8_t  g_fail_step;
static bool     g_multi_active;
static uint32_t g_mb_count;   /* blocks written in current CMD25 session */
static uint8_t  g_mb_fail;    /* 0=none 1=resp-reject 2=busy-timeout */
static uint64_t g_busy_total; /* accumulated busy-wait µs over the session */
static uint32_t g_busy_n;     /* blocks counted for the busy average */

/* ── Clock delay ───────────────────────────────────────────────────────────── */

/* ~4 µs half-period → ~125 kHz, well within 400 kHz eMMC init limit */
static inline void clk_delay(void)
{
	for (volatile int i = 0; i < 64; i++) __ASM volatile ("nop");
}

/* 0 NOPs → ~3 MHz limited by GPIO store latency; THGBMNG5D1LBAIL supports 52 MHz HS */
static inline void fast_clk_delay(void)
{
	__ASM volatile ("");
}

static void clock_pulse(void)
{
	NRF_P0->OUTSET = CLK_MASK;
	clk_delay();
	NRF_P0->OUTCLR = CLK_MASK;
	clk_delay();
}

static void fast_clock_pulse(void)
{
	NRF_P0->OUTSET = CLK_MASK;
	fast_clk_delay();
	NRF_P0->OUTCLR = CLK_MASK;
	fast_clk_delay();
}

/* ── CRC16-CCITT (polynomial 0x1021, init=0, MSB-first) ───────────────────── */

static const uint16_t crc16_table[256] = {
	0x0000,0x1021,0x2042,0x3063,0x4084,0x50A5,0x60C6,0x70E7,
	0x8108,0x9129,0xA14A,0xB16B,0xC18C,0xD1AD,0xE1CE,0xF1EF,
	0x1231,0x0210,0x3273,0x2252,0x52B5,0x4294,0x72F7,0x62D6,
	0x9339,0x8318,0xB37B,0xA35A,0xD3BD,0xC39C,0xF3FF,0xE3DE,
	0x2462,0x3443,0x0420,0x1401,0x64E6,0x74C7,0x44A4,0x5485,
	0xA56A,0xB54B,0x8528,0x9509,0xE5EE,0xF5CF,0xC5AC,0xD58D,
	0x3653,0x2672,0x1611,0x0630,0x76D7,0x66F6,0x5695,0x46B4,
	0xB75B,0xA77A,0x9719,0x8738,0xF7DF,0xE7FE,0xD79D,0xC7BC,
	0x48C4,0x58E5,0x6886,0x78A7,0x0840,0x1861,0x2802,0x3823,
	0xC9CC,0xD9ED,0xE98E,0xF9AF,0x8948,0x9969,0xA90A,0xB92B,
	0x5AF5,0x4AD4,0x7AB7,0x6A96,0x1A71,0x0A50,0x3A33,0x2A12,
	0xDBFD,0xCBDC,0xFBBF,0xEB9E,0x9B79,0x8B58,0xBB3B,0xAB1A,
	0x6CA6,0x7C87,0x4CE4,0x5CC5,0x2C22,0x3C03,0x0C60,0x1C41,
	0xEDAE,0xFD8F,0xCDEC,0xDDCD,0xAD2A,0xBD0B,0x8D68,0x9D49,
	0x7E97,0x6EB6,0x5ED5,0x4EF4,0x3E13,0x2E32,0x1E51,0x0E70,
	0xFF9F,0xEFBE,0xDFDD,0xCFFC,0xBF1B,0xAF3A,0x9F59,0x8F78,
	0x9188,0x81A9,0xB1CA,0xA1EB,0xD10C,0xC12D,0xF14E,0xE16F,
	0x1080,0x00A1,0x30C2,0x20E3,0x5004,0x4025,0x7046,0x6067,
	0x83B9,0x9398,0xA3FB,0xB3DA,0xC33D,0xD31C,0xE37F,0xF35E,
	0x02B1,0x1290,0x22F3,0x32D2,0x4235,0x5214,0x6277,0x7256,
	0xB5EA,0xA5CB,0x95A8,0x8589,0xF56E,0xE54F,0xD52C,0xC50D,
	0x34E2,0x24C3,0x14A0,0x0481,0x7466,0x6447,0x5424,0x4405,
	0xA7DB,0xB7FA,0x8799,0x97B8,0xE75F,0xF77E,0xC71D,0xD73C,
	0x26D3,0x36F2,0x0691,0x16B0,0x6657,0x7676,0x4615,0x5634,
	0xD94C,0xC96D,0xF90E,0xE92F,0x99C8,0x89E9,0xB98A,0xA9AB,
	0x5844,0x4865,0x7806,0x6827,0x18C0,0x08E1,0x3882,0x28A3,
	0xCB7D,0xDB5C,0xEB3F,0xFB1E,0x8BF9,0x9BD8,0xABBB,0xBB9A,
	0x4A75,0x5A54,0x6A37,0x7A16,0x0AF1,0x1AD0,0x2AB3,0x3A92,
	0xFD2E,0xED0F,0xDD6C,0xCD4D,0xBDAA,0xAD8B,0x9DE8,0x8DC9,
	0x7C26,0x6C07,0x5C64,0x4C45,0x3CA2,0x2C83,0x1CE0,0x0CC1,
	0xEF1F,0xFF3E,0xCF5D,0xDF7C,0xAF9B,0xBFBA,0x8FD9,0x9FF8,
	0x6E17,0x7E36,0x4E55,0x5E74,0x2E93,0x3EB2,0x0ED1,0x1EF0,
};

static uint16_t crc16(const uint8_t *data, size_t len)
{
	uint16_t crc = 0;
	for (size_t i = 0; i < len; i++)
		crc = (uint16_t)((crc << 8) ^ crc16_table[(crc >> 8) ^ data[i]]);
	return crc;
}

/* ── CRC7 (polynomial x^7 + x^3 + 1 = 0x09) ───────────────────────────────── */

static uint8_t crc7(const uint8_t *data, size_t len)
{
	uint8_t crc = 0;
	for (size_t i = 0; i < len; i++) {
		uint8_t byte = data[i];
		for (int b = 0; b < 8; b++) {
			crc <<= 1;
			if ((crc ^ byte) & 0x80u) crc ^= 0x09u;
			byte <<= 1;
		}
	}
	return crc & 0x7Fu;
}

/* ── CMD bit-bang ──────────────────────────────────────────────────────────── */

/*
 * Send a 48-bit eMMC command and receive the response.
 *
 * resp_bits: 48 (R1/R3/R6), 136 (R2), 0 (no response)
 * Returns true if response received (or resp_bits==0).
 */
static bool send_command(uint8_t cmd_index, uint32_t argument,
                         uint8_t *resp_buf, int resp_bits, bool skip_idle)
{
	uint8_t frame[6];
	frame[0] = 0x40u | (cmd_index & 0x3Fu);
	frame[1] = (argument >> 24) & 0xFFu;
	frame[2] = (argument >> 16) & 0xFFu;
	frame[3] = (argument >> 8)  & 0xFFu;
	frame[4] =  argument        & 0xFFu;
	frame[5] = (crc7(frame, 5) << 1) | 0x01u;

	/* Transmit 48 bits MSB-first on CMD */
	for (int b = 0; b < 6; b++) {
		for (int bit = 7; bit >= 0; bit--) {
			if ((frame[b] >> bit) & 1u)
				NRF_P0->OUTSET = CMD_MASK;
			else
				NRF_P0->OUTCLR = CMD_MASK;
			clock_pulse();
		}
	}

	if (resp_bits == 0) {
		/* No response — 8 idle clocks then done */
		for (int i = 0; i < 8; i++) clock_pulse();
		return true;
	}

	/* Switch CMD to input, wait for start bit (CMD driven low by card) */
	NRF_P0->PIN_CNF[PIN_CMD] = CNF_INPUT_PULLUP;

	bool got_start = false;
	for (int i = 0; i < 10000; i++) {
		NRF_P0->OUTSET = CLK_MASK;
		clk_delay();
		uint32_t v = (NRF_P0->IN >> PIN_CMD) & 1u;
		NRF_P0->OUTCLR = CLK_MASK;
		clk_delay();
		if (v == 0u) { got_start = true; break; }
	}

	if (!got_start) {
		NRF_P0->PIN_CNF[PIN_CMD] = CNF_OUTPUT_HD;
		NRF_P0->OUTSET = CMD_MASK;
		return false;
	}

	/* Receive resp_bits-1 bits (start bit already consumed) */
	if (resp_buf)
		memset(resp_buf, 0, ((size_t)resp_bits + 7u) / 8u);

	int byte_idx = 0, bit_idx = 6;
	for (int i = 0; i < resp_bits - 1; i++) {
		NRF_P0->OUTSET = CLK_MASK;
		clk_delay();
		uint32_t v = (NRF_P0->IN >> PIN_CMD) & 1u;
		NRF_P0->OUTCLR = CLK_MASK;
		clk_delay();
		if (resp_buf && v)
			resp_buf[byte_idx] |= (uint8_t)(1u << bit_idx);
		if (--bit_idx < 0) { bit_idx = 7; byte_idx++; }
	}

	NRF_P0->PIN_CNF[PIN_CMD] = CNF_OUTPUT_HD;
	NRF_P0->OUTSET = CMD_MASK;

	/* Trailing idle clocks — skipped for data-read commands so the card's
	 * data start bit is not consumed before wait_for_data_start() runs. */
	if (!skip_idle)
		for (int i = 0; i < 16; i++) clock_pulse();

	return true;
}

/* ── High-level eMMC commands ──────────────────────────────────────────────── */

static bool cmd0_go_idle(void)
{
	NRF_P0->OUTSET = CMD_MASK;
	for (int i = 0; i < 80; i++) clock_pulse();
	return send_command(0, 0x00000000u, NULL, 0, false);
}

static bool cmd1_poll(void)
{
	uint8_t resp[6];
	for (int attempt = 0; attempt < 1000; attempt++) {
		feed_wdt();
		if (!send_command(1, 0x40FF8080u, resp, 48, false))
			return false;
		uint32_t ocr = ((uint32_t)resp[1] << 24) | ((uint32_t)resp[2] << 16) |
		               ((uint32_t)resp[3] << 8)  |  (uint32_t)resp[4];
		if (ocr & (1u << 31))
			return true; /* busy bit clear = card ready */
		if (attempt < 30)
			for (volatile int d = 0; d < 6400; d++) __ASM volatile ("nop");
		else
			delay_ms(1);
	}
	return false;
}

/* CMD2: ALL_SEND_CID — 136-bit R2 */
static bool cmd2_all_send_cid(void)
{
	uint8_t resp[18];
	return send_command(2, 0x00000000u, resp, 136, false);
}

/* CMD3: SET_RELATIVE_ADDR — host assigns RCA; card responds R1 (status only) */
static bool cmd3_set_relative_addr(void)
{
	/* 0x5A7E matches the reference driver's hardcoded value */
	g_rca = 0x5A7Eu;
	uint8_t resp[6];
	return send_command(3, (uint32_t)g_rca << 16, resp, 48, false);
}

/* CMD9: SEND_CSD — 136-bit R2; we ignore the data but must advance card state */
static bool cmd9_send_csd(void)
{
	uint8_t resp[18];
	return send_command(9, (uint32_t)g_rca << 16, resp, 136, false);
}

/* CMD7: SELECT_CARD — STAND-BY → TRANSFER; retry up to 100× like reference */
static bool cmd7_select_card(void)
{
	uint8_t resp[6];
	for (int attempt = 0; attempt < 100; attempt++) {
		feed_wdt();
		if (send_command(7, (uint32_t)g_rca << 16, resp, 48, false))
			return true;
		for (volatile int d = 0; d < 6400; d++) __ASM volatile ("nop");
	}
	return false;
}

static bool cmd16_set_blocklen(uint32_t len)
{
	uint8_t resp[6];
	return send_command(16, len, resp, 48, false);
}

static bool cmd13_send_status(uint32_t *status)
{
	uint8_t resp[6];
	if (!send_command(13, (uint32_t)g_rca << 16, resp, 48, false))
		return false;
	if (status)
		*status = ((uint32_t)resp[1] << 24) | ((uint32_t)resp[2] << 16) |
		          ((uint32_t)resp[3] << 8)  |  (uint32_t)resp[4];
	return true;
}

static bool cmd12_stop(void)
{
	uint8_t resp[6];
	return send_command(12, 0x00000000u, resp, 48, false);
}

/* CMD23: SET_BLOCK_COUNT — pre-declare write count before CMD25. */
static bool cmd23_set_block_count(uint32_t count)
{
	uint8_t resp[6];
	return send_command(23, count, resp, 48, false);
}

/* CMD6: SWITCH — modify one EXT_CSD byte. access: 1=set bits, 2=clear bits,
 * 3=write byte. Polls CMD13 to TRANSFER state and checks SWITCH_ERROR (bit 7).
 * Returns true only if the switch was accepted (no SWITCH_ERROR). */
static bool cmd6_access(uint8_t access, uint8_t index, uint8_t value)
{
	uint32_t arg = ((uint32_t)access << 24) | ((uint32_t)index << 16) |
	               ((uint32_t)value << 8);
	uint8_t resp[6];
	if (!send_command(6, arg, resp, 48, false)) return false;
	for (int i = 0; i < 1000; i++) {
		feed_wdt();
		uint32_t status;
		if (!cmd13_send_status(&status)) return false;
		if (((status >> 9) & 0x0Fu) == 4u)            /* reached TRANSFER */
			return ((status >> 7) & 1u) == 0u;        /* false if SWITCH_ERROR */
		for (volatile int d = 0; d < 6400; d++) __ASM volatile ("nop");
	}
	return false;
}

/* Write-byte convenience wrapper (ACCESS = 0x03). */
static bool cmd6_switch(uint8_t index, uint8_t value)
{
	return cmd6_access(0x03u, index, value);
}

/* ── GPIO bit-bang data layer ────────────────────────────────────────────────── */

/*
 * Wait for eMMC data start bit: clock CLK via GPIO and watch DAT0.
 * Returns true with CLK low after detecting start bit (DAT0 low on rising edge).
 */
static bool wait_for_data_start(void)
{
	volatile uint32_t *outset = &NRF_P0->OUTSET;
	volatile uint32_t *outclr = &NRF_P0->OUTCLR;
	const volatile uint32_t *in_reg = &NRF_P0->IN;

	for (uint32_t i = 0; i < 2000000u; i++) {
		*outset = CLK_MASK;
		fast_clk_delay();
		uint32_t dat0 = (*in_reg >> PIN_DAT0) & 1u;
		*outclr = CLK_MASK;
		fast_clk_delay();
		if (dat0 == 0u)
			return true;
		if ((i & 0xFFFFu) == 0u) feed_wdt();
	}
	return false;
}

/* Read len bytes MSB-first from DAT0 via GPIO bit-bang, sampling on rising CLK. */
static void read_bytes_bitbang(uint8_t *buf, uint32_t len)
{
	for (uint32_t b = 0; b < len; b++) {
		uint8_t byte = 0;
		for (int bit = 7; bit >= 0; bit--) {
			NRF_P0->OUTSET = CLK_MASK;
			fast_clk_delay();
			if ((NRF_P0->IN >> PIN_DAT0) & 1u)
				byte |= (uint8_t)(1u << bit);
			NRF_P0->OUTCLR = CLK_MASK;
			fast_clk_delay();
		}
		buf[b] = byte;
	}
}

/* ── CMD8: SEND_EXT_CSD ─────────────────────────────────────────────────────── */

/* Static to avoid blowing the 512-byte main stack. EXT_CSD[215:212] = SEC_COUNT. */
static uint8_t s_ext_csd[512];

static bool cmd8_send_ext_csd(void)
{
	uint8_t resp[6];
	if (!send_command(8, 0x00000000u, resp, 48, true))
		return false;

	if (!wait_for_data_start())
		return false;

	read_bytes_bitbang(s_ext_csd, 512);
	for (int i = 0; i < 16; i++) fast_clock_pulse(); /* drain 2 CRC bytes */

	g_capacity_blocks =
		((uint32_t)s_ext_csd[215] << 24) |
		((uint32_t)s_ext_csd[214] << 16) |
		((uint32_t)s_ext_csd[213] << 8)  |
		 (uint32_t)s_ext_csd[212];

	/* CACHE_SIZE is EXT_CSD[252:249] in KiB (NOT [168]). */
	g_cache_size_kb =
		((uint32_t)s_ext_csd[252] << 24) |
		((uint32_t)s_ext_csd[251] << 16) |
		((uint32_t)s_ext_csd[250] << 8)  |
		 (uint32_t)s_ext_csd[249];

	return true;
}

/* ── Block reads ────────────────────────────────────────────────────────────── */

static bool read_block_single(uint32_t block_addr, uint8_t *buf)
{
	uint8_t resp[6];
	if (!send_command(17, block_addr, resp, 48, true))
		return false;

	if (!wait_for_data_start())
		return false;

	read_bytes_bitbang(buf, 512);
	for (int i = 0; i < 16; i++) fast_clock_pulse(); /* drain 2 CRC bytes */
	for (int i = 0; i < 16; i++) fast_clock_pulse(); /* idle before next command */
	return true;
}

/* ── SPIM3 16 MHz data burst ─────────────────────────────────────────────────
 * SPIM3 (0x4002F000) drives CLK+DAT0 via EasyDMA for the 515-byte data write
 * in CMD25 blocks.  All other phases (CMD, response, busy) stay GPIO bit-bang.
 * Buffer must be in RAM for EasyDMA — static .bss satisfies this.
 * Disconnect value 0xFFFFFFFF = CONNECT bit set (nRF52840 PSEL encoding).
 * ─────────────────────────────────────────────────────────────────────────── */

static uint8_t s_spim_tx[515]; /* [0xFE][data 512][crc_hi][crc_lo] */

static void spim3_init(void)
{
	NRF_SPIM3->ENABLE    = 0u;
	NRF_SPIM3->PSEL.SCK  = 0xFFFFFFFFu;
	NRF_SPIM3->PSEL.MOSI = 0xFFFFFFFFu;
	NRF_SPIM3->PSEL.MISO = 0xFFFFFFFFu;
	NRF_SPIM3->PSEL.CSN  = 0xFFFFFFFFu;
	NRF_SPIM3->FREQUENCY = 0x0A000000u;  /* 16 MHz, SPIM3-only value */
	NRF_SPIM3->CONFIG    = 0u;            /* mode 0 (CPOL=0/CPHA=0), MSB first */
	NRF_SPIM3->ORC       = 0xFFu;
}

/* ── Public API ────────────────────────────────────────────────────────────── */

bool emmc_init(void)
{
	g_rca             = 0;
	g_capacity_blocks = 0;
	g_initialized     = false;
	g_fail_step       = 0;

	/* Drive all eMMC I/O pins to safe state before power cycle */
	NRF_P0->PIN_CNF[PIN_CLK]  = CNF_OUTPUT_HD;
	NRF_P0->OUTCLR = CLK_MASK;
	NRF_P0->PIN_CNF[PIN_CMD]  = CNF_OUTPUT_HD;
	NRF_P0->OUTCLR = CMD_MASK;
	NRF_P0->PIN_CNF[PIN_DAT0] = CNF_OUTPUT_HD;
	NRF_P0->OUTCLR = DAT0_MASK;

	/* VCCQ (P0.14): full power cycle to guarantee clean card reset */
	NRF_P0->PIN_CNF[PIN_VCCQ] = CNF_OUTPUT_HD;
	NRF_P0->OUTCLR = (1u << PIN_VCCQ);  /* VCCQ off */
	delay_ms(10);
	NRF_P0->OUTSET = (1u << PIN_VCCQ);  /* VCCQ on */
	delay_ms(5);  /* supply stabilise */

	/* RST (P1.08): assert then release reset */
	NRF_P1->PIN_CNF[PIN_RST] = GPIO_OUT_CNF;
	NRF_P1->OUTCLR = (1u << PIN_RST);
	delay_ms(1);
	NRF_P1->OUTSET = (1u << PIN_RST);
	delay_ms(10); /* card boot */

	/* CLK idle low, CMD idle high, DAT0 input pull-up */
	NRF_P0->PIN_CNF[PIN_CMD]  = CNF_OUTPUT_HD;
	NRF_P0->OUTSET = CMD_MASK;
	NRF_P0->PIN_CNF[PIN_DAT0] = CNF_INPUT_PULLUP;

	/* 5ms settle after pin config */
	delay_ms(5);

#define FAIL(step) do { g_fail_step = (step); return false; } while (0)

	if (!cmd0_go_idle())           FAIL(1);
	if (!cmd1_poll())              FAIL(2);
	if (!cmd2_all_send_cid())      FAIL(3);
	if (!cmd3_set_relative_addr()) FAIL(4);
	delay_ms(5); /* extra settle before CMD9 */
	if (!cmd9_send_csd())          FAIL(5);
	if (!cmd7_select_card())       FAIL(6);

	if (!cmd8_send_ext_csd())      FAIL(7);
	if (!cmd16_set_blocklen(512))  FAIL(8);

	spim3_init();

	/* Disable reliable write for the user data area when the card lets the host
	 * control it (WR_REL_PARAM[166] bit 2 = EN_REL_WR).  Reliable write forces a
	 * synchronous NAND commit per block (~2.35ms busy), defeating the write cache.
	 * WR_REL_SET[167] bit 0 = user area; write 0 → non-reliable → cache write-back.
	 * Must precede cache enable. */
	g_rel_write_off = false;
	if (s_ext_csd[166] & 0x04u) {
		/* Try Clear-Bits (0x02) on WR_REL_SET bit 0 — some cards reject the
		 * Write-Byte form but honor clear-bits. Fall back to Write-Byte. */
		g_rel_write_off = cmd6_access(0x02u, 167, 0x01u);
		if (!g_rel_write_off)
			g_rel_write_off = cmd6_switch(167, 0);
	}

	/* Enable write cache (volatile — cleared by VCCQ power cycle each init).
	 * With reliable-write off, the cache can hold blocks and program NAND in the
	 * background, so per-block busy drops far below the write-through ~2.35ms.
	 * CMD12/CMD6-FLUSH at end of CMD25 commits the cache before close. */
	/* Enable write cache: blocks return fast (cached) while NAND programs in the
	 * background — key for upload throughput. (The earlier "wedge at 18784" was a
	 * main-stack overflow, not a cache problem.) CMD12/FLUSH commits before close. */
	g_cache_enabled = false;
	if (g_cache_size_kb > 0)
		g_cache_enabled = cmd6_switch(33, 1);

	/* Re-read EXT_CSD so emmc_ext_csd() reflects the post-write register state
	 * (lets `rome extcsd` confirm CACHE_CTRL/WR_REL_SET actually changed). */
	cmd8_send_ext_csd();

	uint32_t status = 0;
	cmd13_send_status(&status);
	if (((status >> 9) & 0x0Fu) != 4u) FAIL(9);

#undef FAIL

	g_initialized = true;
	return true;
}

uint8_t emmc_fail_step(void)
{
	return g_fail_step;
}

bool emmc_read_block(uint32_t block_addr, uint8_t *buf)
{
	if (!g_initialized || !buf) return false;
	if (block_addr >= g_capacity_blocks) return false;
	return read_block_single(block_addr, buf);
}

bool emmc_read_blocks(uint32_t block_addr, uint8_t *buf, uint32_t num_blocks)
{
	if (!g_initialized || !buf || num_blocks == 0) return false;
	if ((uint64_t)block_addr + num_blocks > (uint64_t)g_capacity_blocks) return false;
	if (num_blocks == 1) return read_block_single(block_addr, buf);

	uint8_t resp[6];
	if (!send_command(18, block_addr, resp, 48, true))
		return false;

	for (uint32_t i = 0; i < num_blocks; i++) {
		if (!wait_for_data_start()) goto fail;
		read_bytes_bitbang(buf + i * 512u, 512);
		for (int j = 0; j < 16; j++) fast_clock_pulse(); /* drain 2 CRC bytes */
	}

	for (int i = 0; i < 16; i++) clock_pulse();
	cmd12_stop();
	return true;

fail:
	cmd12_stop();
	return false;
}

uint32_t emmc_capacity_blocks(void)
{
	return g_capacity_blocks;
}

uint32_t emmc_cache_size_kb(void)
{
	return g_cache_size_kb;
}

bool emmc_cache_enabled(void)
{
	return g_cache_enabled;
}

const uint8_t *emmc_ext_csd(void)
{
	return s_ext_csd;
}


/* ── Block write (CMD24, bit-bang DAT0) ─────────────────────────────────────── */

static void write_byte_dat0(uint8_t byte)
{
	for (int bit = 7; bit >= 0; bit--) {
		if ((byte >> bit) & 1u)
			NRF_P0->OUTSET = DAT0_MASK;
		else
			NRF_P0->OUTCLR = DAT0_MASK;
		NRF_P0->OUTSET = CLK_MASK;
		fast_clk_delay();
		NRF_P0->OUTCLR = CLK_MASK;
		fast_clk_delay();
	}
}

bool emmc_write_multi_active(void)
{
	return g_multi_active;
}

bool emmc_cache_flush(void)
{
	if (!g_initialized) return false;
	/* EXT_CSD[32] FLUSH_CACHE: write 1 triggers flush; CMD6 polls until done */
	return cmd6_switch(32, 1);
}

/* ── CMD25 streaming multi-block write ──────────────────────────────────────── */

bool emmc_write_multi_begin(uint32_t block_addr, uint32_t num_blocks)
{
	if (!g_initialized || g_multi_active) return false;
	if (block_addr >= g_capacity_blocks) return false;

	/* Set BEFORE any CMD access so fill_block's !emmc_write_multi_active()
	 * check prevents concurrent CMD17 reads from the audio feed thread. */
	g_multi_active = true;

	/* CMD23 pre-declares the write count */
	if (num_blocks > 0) {
		if (!cmd23_set_block_count(num_blocks)) { g_multi_active = false; return false; }
	}

	uint8_t resp[6];
	if (!send_command(25, block_addr, resp, 48, false)) { g_multi_active = false; return false; }

	NRF_P0->OUTSET = DAT0_MASK;
	NRF_P0->PIN_CNF[PIN_DAT0] = CNF_OUTPUT_HD;
	g_mb_count = 0;
	g_mb_fail  = 0;
	g_busy_total = 0;
	g_busy_n = 0;
	return true;
}

uint32_t emmc_mb_count(void) { return g_mb_count; }
uint8_t  emmc_mb_fail(void)  { return g_mb_fail; }
uint32_t emmc_busy_us(void)  { return g_busy_n ? (uint32_t)(g_busy_total / g_busy_n) : 0u; }

bool emmc_write_multi_block(const uint8_t *buf)
{
	if (!g_multi_active || !buf) return false;

	/* Pack TX buffer: start token + data + CRC16.
	 * Compute CRC first so it's ready before SPIM3 starts. */
	uint16_t crc = crc16(buf, 512);
	s_spim_tx[0]   = 0xFEu;
	memcpy(s_spim_tx + 1, buf, 512);
	s_spim_tx[513] = (uint8_t)(crc >> 8);
	s_spim_tx[514] = (uint8_t)(crc & 0xFFu);

	/* Blast 515 bytes via SPIM3 at 16 MHz (~257 µs).
	 * CLK idles low (mode 0), MOSI drives DAT0 MSB-first — correct for native eMMC. */
	NRF_SPIM3->PSEL.SCK   = PIN_CLK;   /* P0.06 = port 0, pin 6 */
	NRF_SPIM3->PSEL.MOSI  = PIN_DAT0;  /* P0.07 = port 0, pin 7 */
	NRF_SPIM3->TXD.PTR    = (uint32_t)s_spim_tx;
	NRF_SPIM3->TXD.MAXCNT = 515u;
	NRF_SPIM3->RXD.MAXCNT = 0u;
	NRF_SPIM3->EVENTS_END  = 0u;
	NRF_SPIM3->ENABLE      = 7u;
	NRF_SPIM3->TASKS_START = 1u;
	while (!NRF_SPIM3->EVENTS_END);
	NRF_SPIM3->ENABLE      = 0u;
	NRF_SPIM3->PSEL.SCK   = 0xFFFFFFFFu;
	NRF_SPIM3->PSEL.MOSI  = 0xFFFFFFFFu;

	/* Read data response token on DAT0.
	 * Scan with slow clocks (pull-up settle + eMMC clock) reading on every edge.
	 * Matches CMD24 response scan — avoids missing start bit in silent-clock window. */
	NRF_P0->PIN_CNF[PIN_DAT0] = CNF_INPUT_PULLUP;

	bool got_resp = false;
	uint8_t status = 0;
	for (int i = 0; i < 16 && !got_resp; i++) {
		NRF_P0->OUTSET = CLK_MASK;
		clk_delay();
		uint32_t bit = (NRF_P0->IN >> PIN_DAT0) & 1u;
		NRF_P0->OUTCLR = CLK_MASK;
		clk_delay();
		if (!bit) {
			status = 0;
			for (int j = 0; j < 4; j++) {
				NRF_P0->OUTSET = CLK_MASK;
				clk_delay();
				status = (uint8_t)((status << 1) | ((NRF_P0->IN >> PIN_DAT0) & 1u));
				NRF_P0->OUTCLR = CLK_MASK;
				clk_delay();
			}
			got_resp = true;
		}
	}
	if (!got_resp || ((status >> 1) & 0x07u) != 0x02u) {
		/* Leave DAT0 as input; emmc_write_multi_end will switch to output */
		g_mb_fail = 1;   /* data response rejected */
		return false;
	}

	/* Wait for the card to finish programming this block.
	 * The card asserts busy (DAT0 low) a few clocks AFTER the data-response
	 * end bit. If we only scanned for DAT0-high we could read "done" before
	 * busy even asserts and fire the next block's burst into a busy card →
	 * intermittent corruption. So: first confirm busy (DAT0 low), then wait
	 * for release (DAT0 high). ~4M iterations ≈ ~1 s ceiling (NAND program +
	 * occasional GC is well under that; longer = wedged card). */
	uint32_t bt0 = k_cycle_get_32();
	for (int i = 0; i < 64; i++) {
		NRF_P0->OUTSET = CLK_MASK; fast_clk_delay();
		uint32_t low = !((NRF_P0->IN >> PIN_DAT0) & 1u);
		NRF_P0->OUTCLR = CLK_MASK; fast_clk_delay();
		if (low) break;   /* busy asserted */
	}
	bool done = false;
	for (uint32_t t = 0; t < 4000000u; t++) {
		NRF_P0->OUTSET = CLK_MASK;
		fast_clk_delay();
		if ((NRF_P0->IN >> PIN_DAT0) & 1u) done = true;
		NRF_P0->OUTCLR = CLK_MASK;
		fast_clk_delay();
		if (done) break;
		if ((t & 0x3FFFFu) == 0u) feed_wdt();
	}
	g_busy_total += k_cyc_to_us_floor32(k_cycle_get_32() - bt0);
	g_busy_n++;

	/* DAT0 back to output + 2 Nwr idle clocks */
	NRF_P0->OUTSET = DAT0_MASK;
	NRF_P0->PIN_CNF[PIN_DAT0] = CNF_OUTPUT_HD;
	NRF_P0->OUTSET = CLK_MASK; fast_clk_delay(); NRF_P0->OUTCLR = CLK_MASK; fast_clk_delay();
	NRF_P0->OUTSET = CLK_MASK; fast_clk_delay(); NRF_P0->OUTCLR = CLK_MASK; fast_clk_delay();

	if (!done) g_mb_fail = 2;   /* busy never cleared */
	else       g_mb_count++;
	return done;
}

bool emmc_write_multi_end(void)
{
	if (!g_multi_active) return false;

	/* Ensure DAT0 is output (handles error-path where block left it as input) */
	NRF_P0->OUTSET = DAT0_MASK;
	NRF_P0->PIN_CNF[PIN_DAT0] = CNF_OUTPUT_HD;

	/* Stop tran token (0xFD) — sent in place of the next 0xFC */
	write_byte_dat0(0xFDu);

	/* Switch to input; give card time to assert busy */
	NRF_P0->PIN_CNF[PIN_DAT0] = CNF_INPUT_PULLUP;
	for (int i = 0; i < 16; i++) fast_clock_pulse();

	bool done = false;
	for (uint32_t t = 0; t < 4000000u; t++) {
		NRF_P0->OUTSET = CLK_MASK;
		fast_clk_delay();
		if ((NRF_P0->IN >> PIN_DAT0) & 1u) done = true;
		NRF_P0->OUTCLR = CLK_MASK;
		fast_clk_delay();
		if (done) break;
		if ((t & 0x3FFFFu) == 0u) feed_wdt();
	}

	for (int i = 0; i < 16; i++) fast_clock_pulse();
	cmd12_stop();
	g_multi_active = false;
	/* Flush write cache to NAND so data survives a power cycle */
	if (g_cache_enabled)
		cmd6_switch(32, 1);
	return done;
}

/* ── CMD24 single-block write ────────────────────────────────────────────────── */

bool emmc_write_block(uint32_t block_addr, const uint8_t *buf)
{
	if (!g_initialized || !buf) return false;
	if (block_addr >= g_capacity_blocks) return false;

	uint8_t resp[6];
	if (!send_command(24, block_addr, resp, 48, false)) return false;

	/* Switch DAT0 to output, idle high */
	NRF_P0->OUTSET = DAT0_MASK;
	NRF_P0->PIN_CNF[PIN_DAT0] = CNF_OUTPUT_HD;

	/* 8 idle clocks (Nwr timing) before data */
	for (int i = 0; i < 8; i++) clock_pulse();

	/* Start token 0xFE */
	write_byte_dat0(0xFEu);

	/* 512 data bytes */
	for (int i = 0; i < 512; i++)
		write_byte_dat0(buf[i]);

	/* CRC16-CCITT over the 512 data bytes — mandatory in native bus mode */
	uint16_t crc = crc16(buf, 512);
	write_byte_dat0((uint8_t)(crc >> 8));
	write_byte_dat0((uint8_t)(crc & 0xFFu));

	/* Switch DAT0 back to input to read data response token.
	 * Token format on DAT0: [0 S S S 1] (5 bits).
	 * N_WR (latency before start bit) is 1–8 clocks — scan for start bit. */
	NRF_P0->PIN_CNF[PIN_DAT0] = CNF_INPUT_PULLUP;

	bool got_resp = false;
	uint8_t status = 0;
	for (int i = 0; i < 16 && !got_resp; i++) {
		NRF_P0->OUTSET = CLK_MASK;
		clk_delay();
		uint32_t bit = (NRF_P0->IN >> PIN_DAT0) & 1u;
		NRF_P0->OUTCLR = CLK_MASK;
		clk_delay();
		if (!bit) {
			/* Found start bit — read 3 status bits then end bit */
			status = 0;
			for (int j = 0; j < 4; j++) {
				NRF_P0->OUTSET = CLK_MASK;
				clk_delay();
				status = (uint8_t)((status << 1) | ((NRF_P0->IN >> PIN_DAT0) & 1u));
				NRF_P0->OUTCLR = CLK_MASK;
				clk_delay();
			}
			got_resp = true;
		}
	}
	/* status = [S2 S1 S0 end]; SSS=010 → accepted */
	if (!got_resp || ((status >> 1) & 0x07u) != 0x02u) {
		for (int i = 0; i < 16; i++) clock_pulse();
		return false;
	}

	/* Wait for not-busy: DAT0 high = card finished writing */
	bool done = false;
	for (uint32_t t = 0; t < 2000000u; t++) {
		NRF_P0->OUTSET = CLK_MASK;
		fast_clk_delay();
		if ((NRF_P0->IN >> PIN_DAT0) & 1u) done = true;
		NRF_P0->OUTCLR = CLK_MASK;
		fast_clk_delay();
		if (done) break;
	}

	for (int i = 0; i < 16; i++) fast_clock_pulse();
	return done;
}
