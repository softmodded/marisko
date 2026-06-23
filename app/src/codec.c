#include "codec.h"
#include "util.h"

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>

/* ── Pins (raw GPIO) ─────────────────────────────────────────────────────────── */
#define PIN_OSC_EN   13u   /* P0.13 — 3.072 MHz oscillator enable (active high) */
#define PIN_CS_RST   15u   /* P0.15 — CS42L42 reset (active low) */
#define PIN_TAS_RST   9u   /* P0.09 — TAS2505 reset (active low, NFC1) */

#define CS_ADDR  0x48u
#define TAS_ADDR 0x18u

/* ── CS42L42 registers (16-bit: page = hi byte, offset = lo byte) ────────────── */
#define CS_DEVID_AB        0x1001u
#define CS_DEVID_CD        0x1002u
#define CS_DEVID_E         0x1003u
#define CS_CLOCK_CTL       0x1007u
#define CS_MCLK_CTL        0x1009u
#define CS_PWR_CTL1        0x1101u
#define CS_PWR_CTL7        0x1107u
#define CS_CODEC_CTL       0x1121u
#define CS_HS_CLAMP_DIS    0x1129u
#define CS_MCLK_SRC_SEL    0x1201u
#define CS_FSYNC_PW_LOWER  0x1203u
#define CS_FSYNC_PERIOD_LO 0x1205u
#define CS_ASP_CLK_CFG     0x1207u
#define CS_ASP_FRM_CFG     0x1208u
#define CS_IN_ASRC_CLK     0x120Au
#define CS_OUT_ASRC_CLK    0x120Bu
#define CS_PLL_LOCK_STATUS 0x130Eu
#define CS_PLL_CTL1        0x1501u
#define CS_PLL_DIV_FRAC2   0x1504u
#define CS_PLL_DIV_INT     0x1505u
#define CS_PLL_CTL3        0x1508u
#define CS_PLL_CAL_RATIO   0x150Au
#define CS_MISC_DET_CTL    0x1B74u
#define CS_MIC_DET_CTL1    0x1B75u
#define CS_TIPSENSE_CTL    0x1B73u
#define CS_HP_CTL          0x2001u
#define CS_MIXER_CHA_VOL   0x2301u
#define CS_MIXER_CHB_VOL   0x2303u
#define CS_DAC_CTL         0x240Eu
#define CS_HP_VOL_A        0x2601u
#define CS_HP_VOL_B        0x2609u
#define CS_ASP_RX_ENABLE   0x2A01u
#define CS_ASP_RX_CH1_RES  0x2A02u
#define CS_ASP_RX_CH2_RES  0x2A05u
#define CS_FINAL_HP_VOL    0x1F06u
#define CS_OSC_SW_STATUS   0x1109u   /* oscillator switch status; 0x02 = SCLK→MCLK */
#define CS_DETECT_STATUS1  0x1B77u   /* bit7 = headphone connected (stock detect) */

/* HP_CTL (0x2001) per the stock TKT-wiki sequence (the one that actually plays
 * audio): 0x0D = mute all, 0x01 = unmute headphones. (The sp1-midi driver's
 * 0x0E/0x02 hold a different control bit and produce no output here.) */
#define CS_HP_CTL_MUTED    0x0Du
#define CS_HP_CTL_UNMUTED  0x01u

struct cs_rw { uint16_t reg; uint8_t val; };

static const struct cs_rw cs_pre_power[] = {
	{CS_MISC_DET_CTL, 0x03},
};
static const struct cs_rw cs_main[] = {
	{CS_PLL_CTL3,      0x10},
	{CS_PLL_DIV_FRAC2, 0x80},   /* stock TKT-wiki value; sp1-midi's 0x00 locked the
	                            * PLL to a slightly-off freq so the MCLK never
	                            * switched off the RCO (0x1109 stuck at 0x00) →
	                            * headphone DAC clocked by RCO → clicks/stutter. */
	{CS_PLL_DIV_INT,   0x3E},
	{CS_PLL_CAL_RATIO, 0x7D},
	{CS_MCLK_CTL,      0x00},
	{CS_MCLK_SRC_SEL,  0x01},
	{CS_IN_ASRC_CLK,   0x01},
	{CS_OUT_ASRC_CLK,  0x01},
	{CS_PLL_CTL1,      0x01},
};
static const struct cs_rw cs_post_power[] = {
	{CS_PWR_CTL7, 0x01},
};
static const struct cs_rw cs_route[] = {
	{CS_CLOCK_CTL,      0x13},
	{CS_FSYNC_PW_LOWER, 0x1F},
	{CS_FSYNC_PERIOD_LO,0x3F},
	{CS_ASP_CLK_CFG,    0x34},
	{CS_ASP_FRM_CFG,    0x1A},
	{CS_ASP_RX_CH1_RES, 0x02},
	{CS_ASP_RX_CH2_RES, 0x42},
	{CS_HP_VOL_A,       0x4C},
	{CS_HP_VOL_B,       0x4C},
	{CS_ASP_RX_ENABLE,  0x0C},
	{CS_DAC_CTL,        0x01},
	{CS_MIXER_CHA_VOL,  0x00},
	{CS_MIXER_CHB_VOL,  0x00},
	{CS_PWR_CTL1,       0x96},
	{CS_CODEC_CTL,      0x41},
	{CS_MIC_DET_CTL1,   0x9F},
	{CS_HS_CLAMP_DIS,   0x01},
	{CS_HP_CTL,         CS_HP_CTL_MUTED},
};
static const struct cs_rw cs_final[] = {
	{CS_FINAL_HP_VOL,  0x86},
	{CS_TIPSENSE_CTL,  0xC0},   /* bits[1:0]=00: no HW tip-sense debounce (was 0x02
	                            * ≈ 500 ms, the plug/unplug lag). Software debounce
	                            * (2 jack reads ≈ 36 ms) handles mechanical bounce. */
};

/* ── TAS2505 init script (page, reg, val) ───────────────────────────────────── */
struct tas_biquad { uint8_t reg, b0, b1, b2; };
static const struct tas_biquad tas_eq[] = {
	{0x0C, 0x7B, 0xEB, 0x19}, {0x10, 0x84, 0x14, 0xE7}, {0x14, 0x7B, 0xEB, 0x19},
	{0x18, 0x7B, 0xDA, 0x72}, {0x1C, 0x88, 0x08, 0x7D}, {0x20, 0x69, 0x43, 0xC5},
	{0x24, 0x9A, 0xB2, 0xD2}, {0x28, 0x62, 0xEA, 0xBF}, {0x2C, 0x65, 0x4D, 0x2E},
	{0x30, 0xB3, 0xD1, 0x7B}, {0x34, 0x5B, 0xD8, 0xC0}, {0x38, 0xB4, 0x2B, 0x3E},
	{0x3C, 0x48, 0x7C, 0x93}, {0x40, 0x6D, 0x32, 0x5F}, {0x44, 0x93, 0x5C, 0x2D},
	{0x48, 0x60, 0x54, 0x42}, {0x4C, 0xB1, 0xFB, 0x66}, {0x50, 0x57, 0x7C, 0x90},
	{0x54, 0x4E, 0x04, 0x9A}, {0x58, 0xC8, 0x2F, 0x2D}, {0x5C, 0x25, 0x68, 0x53},
	{0x60, 0x25, 0x68, 0x53}, {0x64, 0x25, 0x68, 0x53}, {0x68, 0xF1, 0xD3, 0xE7},
	{0x6C, 0x06, 0xB6, 0xE3},
};

/* ── State ───────────────────────────────────────────────────────────────────── */
static const struct device *s_i2c;
static int      s_cs_page  = -1;
static int      s_tas_page = -1;
static codec_diag_t s_diag;

/* ── Low-level GPIO ──────────────────────────────────────────────────────────── */
static void gpio_out(uint32_t pin, int high)
{
	NRF_P0->PIN_CNF[pin] = GPIO_OUT_CNF;
	if (high) NRF_P0->OUTSET = (1u << pin);
	else      NRF_P0->OUTCLR = (1u << pin);
}

/* ── I2C helpers (count failures into the diag) ──────────────────────────────── */
static bool cs_write(uint16_t reg, uint8_t val)
{
	uint8_t page = (uint8_t)(reg >> 8);
	uint8_t off  = (uint8_t)(reg & 0xFF);
	if ((int)page != s_cs_page) {
		uint8_t pb[2] = {0x00, page};
		if (i2c_write(s_i2c, pb, 2, CS_ADDR) != 0) { s_diag.i2c_errors++; return false; }
		s_cs_page = page;
	}
	uint8_t db[2] = {off, val};
	if (i2c_write(s_i2c, db, 2, CS_ADDR) != 0) { s_diag.i2c_errors++; return false; }
	return true;
}

static bool cs_read(uint16_t reg, uint8_t *val)
{
	uint8_t page = (uint8_t)(reg >> 8);
	uint8_t off  = (uint8_t)(reg & 0xFF);
	if ((int)page != s_cs_page) {
		uint8_t pb[2] = {0x00, page};
		if (i2c_write(s_i2c, pb, 2, CS_ADDR) != 0) { s_diag.i2c_errors++; return false; }
		s_cs_page = page;
	}
	if (i2c_write_read(s_i2c, CS_ADDR, &off, 1, val, 1) != 0) { s_diag.i2c_errors++; return false; }
	return true;
}

static bool tas_write(uint8_t page, uint8_t reg, uint8_t val)
{
	if ((int)page != s_tas_page) {
		uint8_t pb[2] = {0x00, page};
		if (i2c_write(s_i2c, pb, 2, TAS_ADDR) != 0) { s_diag.i2c_errors++; return false; }
		s_tas_page = page;
	}
	uint8_t db[2] = {reg, val};
	if (i2c_write(s_i2c, db, 2, TAS_ADDR) != 0) { s_diag.i2c_errors++; return false; }
	return true;
}

static bool tas_read(uint8_t page, uint8_t reg, uint8_t *val)
{
	if ((int)page != s_tas_page) {
		uint8_t pb[2] = {0x00, page};
		if (i2c_write(s_i2c, pb, 2, TAS_ADDR) != 0) { s_diag.i2c_errors++; return false; }
		s_tas_page = page;
	}
	if (i2c_write_read(s_i2c, TAS_ADDR, &reg, 1, val, 1) != 0) { s_diag.i2c_errors++; return false; }
	return true;
}

static bool cs_run(const struct cs_rw *s, int n)
{
	bool ok = true;
	for (int i = 0; i < n; i++) ok = cs_write(s[i].reg, s[i].val) && ok;
	return ok;
}

/* ── Public ──────────────────────────────────────────────────────────────────── */

static bool cs42l42_bringup(void)
{
	s_cs_page = -1;

	/* Reset pulse: assert low 1ms, release high, wait 10ms. */
	gpio_out(PIN_CS_RST, 0);
	delay_ms(1);
	gpio_out(PIN_CS_RST, 1);
	delay_ms(10);

	/* Probe DEVID (record but don't abort — we want full diag either way). */
	cs_read(CS_DEVID_AB, &s_diag.cs_devid_ab);
	cs_read(CS_DEVID_CD, &s_diag.cs_devid_cd);
	cs_read(CS_DEVID_E,  &s_diag.cs_devid_e);

	bool ok = true;
	ok = cs_run(cs_pre_power, ARRAY_SIZE(cs_pre_power)) && ok;
	delay_ms(1);
	ok = cs_run(cs_main, ARRAY_SIZE(cs_main)) && ok;
	delay_ms(1);
	ok = cs_run(cs_post_power, ARRAY_SIZE(cs_post_power)) && ok;

	/* After PWR_CTL7 (oscillator switch control), wait for the codec to switch
	 * its internal MCLK over to SCLK before configuring the serial port. Stock
	 * power-up (TKT wiki, CS42L42 datasheet p.94) polls 0x1109 until 0x02; the
	 * DAC clock domain isn't ready until this completes (skipping it leaves the
	 * headphone DAC unclocked → silent). Bounded retries so we never hang. */
	for (int i = 0; i < 10; i++) {
		uint8_t sw = 0;
		if (cs_read(CS_OSC_SW_STATUS, &sw) && sw == 0x02) break;
		delay_ms(1);
	}

	/* Start the playback clock tree (this is what makes LRCLK appear). */
	ok = cs_run(cs_route, ARRAY_SIZE(cs_route)) && ok;
	delay_ms(22);
	ok = cs_run(cs_final, ARRAY_SIZE(cs_final)) && ok;

	cs_read(CS_PLL_LOCK_STATUS, &s_diag.cs_pll_lock);
	cs_read(CS_CLOCK_CTL,       &s_diag.cs_clock_ctl);
	cs_read(CS_PWR_CTL1,        &s_diag.cs_pwr_ctl1);
	cs_read(CS_HP_CTL,          &s_diag.cs_hp_ctl);
	return ok;
}

static bool tas2505_bringup(void)
{
	s_tas_page = -1;

	/* Reset pulse: assert low 2ms, release, 2ms. */
	gpio_out(PIN_TAS_RST, 0);
	delay_ms(2);
	gpio_out(PIN_TAS_RST, 1);
	delay_ms(2);

	bool ok = true;
	ok = tas_write(0, 1, 0x01) && ok;   /* software reset */
	delay_ms(2);

	ok = tas_write(1, 2, 0x04) && ok;
	ok = tas_write(0, 4, 0x07) && ok;   /* clock mux: PLL from BCLK */
	ok = tas_write(0, 5, 0x94) && ok;
	ok = tas_write(0, 6, 0x07) && ok;
	ok = tas_write(0, 7, 0x00) && ok;
	ok = tas_write(0, 8, 0x00) && ok;
	delay_ms(15);                        /* PLL settle */

	ok = tas_write(0, 11, 0x82) && ok;
	ok = tas_write(0, 12, 0x87) && ok;
	ok = tas_write(0, 13, 0x00) && ok;
	ok = tas_write(0, 14, 0x80) && ok;
	ok = tas_write(0, 27, 0x20) && ok;  /* P0/R27: D7-6=00 I2S, D5-4=10 24-bit, D3=0 BCLK slave */
	ok = tas_write(0, 28, 0x00) && ok;
	ok = tas_write(0, 60, 0x03) && ok;

	/* DSP coefficient table on page 44 (raw 4-byte writes). */
	if (ok && (int)44 != s_tas_page) {
		uint8_t pb[2] = {0x00, 44};
		if (i2c_write(s_i2c, pb, 2, TAS_ADDR) != 0) { s_diag.i2c_errors++; ok = false; }
		else s_tas_page = 44;
	}
	for (size_t i = 0; i < ARRAY_SIZE(tas_eq) && ok; i++) {
		uint8_t b[4] = {tas_eq[i].reg, tas_eq[i].b0, tas_eq[i].b1, tas_eq[i].b2};
		if (i2c_write(s_i2c, b, 4, TAS_ADDR) != 0) { s_diag.i2c_errors++; ok = false; }
	}

	ok = tas_write(1, 1, 0x10) && ok;
	ok = tas_write(1, 10, 0x00) && ok;
	ok = tas_write(1, 12, 0x04) && ok;
	ok = tas_write(1, 24, 0x80) && ok;
	ok = tas_write(1, 46, 0x7F) && ok;  /* speaker volume = mute */

	tas_read(0, 2,  &s_diag.tas_p0r2);
	tas_read(1, 46, &s_diag.tas_p1r46);
	tas_read(1, 45, &s_diag.tas_p1r45);
	tas_read(0, 63, &s_diag.tas_p0r63);
	return ok;
}

bool codec_init(void)
{
	for (uint8_t i = 0; i < sizeof(s_diag); i++) ((uint8_t *)&s_diag)[i] = 0;

	s_i2c = DEVICE_DT_GET(DT_NODELABEL(i2c0));
	if (!device_is_ready(s_i2c)) {
		s_diag.init_ok = 0;
		return false;
	}

	/* Enable the 3.072 MHz oscillator before releasing codec resets. */
	gpio_out(PIN_OSC_EN, 1);
	delay_ms(2);
	feed_wdt();

	bool ok = cs42l42_bringup();
	feed_wdt();
	ok = tas2505_bringup() && ok;
	feed_wdt();

	s_diag.init_ok = (ok && s_diag.i2c_errors == 0) ? 1 : 0;
	return ok;
}

bool codec_speaker_mute(bool mute)
{
	s_tas_page = -1;
	if (mute) {
		return tas_write(1, 46, 0x7F) &&
		       tas_write(0, 63, 0x30) &&
		       tas_write(1, 45, 0x00);
	}
	return tas_write(1, 46, 0x00) &&
	       tas_write(1, 48, 0x50) &&
	       tas_write(1, 45, 0x02) &&
	       tas_write(0, 63, 0xB0) &&
	       tas_write(0, 65, 0x0A) &&
	       tas_write(0, 64, 0x04);
}

bool codec_speaker_volume(uint8_t vol)
{
	s_tas_page = -1;
	return tas_write(1, 46, vol);
}

bool codec_headphone_mute(bool mute)
{
	s_cs_page = -1;
	return cs_write(CS_HP_CTL, mute ? CS_HP_CTL_MUTED : CS_HP_CTL_UNMUTED);
}

bool codec_headphone_volume(uint8_t vol)
{
	/* Digital mixer input volume (CS42L42 MIXER_CHA/CHB_VOL, 6-bit two's-comp:
	 * 0x00 = 0 dB, 0x3F = -1 dB … i.e. larger = quieter). NOTE: 0x2601/0x2609
	 * are the SRC sample-rate regs in the stock config, NOT volume — must stay
	 * 0x4C — so headphone level is controlled here at the mixer instead. */
	s_cs_page = -1;
	return cs_write(CS_MIXER_CHA_VOL, vol) && cs_write(CS_MIXER_CHB_VOL, vol);
}

bool codec_headphones_present(void)
{
	/* Stock detect: Detect Status 1 (0x1B77) bit7 = headphone connected
	 * (datasheet 7.9.8). Clean level status — no debounce/latch quirks. */
	s_cs_page = -1;
	uint8_t v = 0;
	if (!cs_read(CS_DETECT_STATUS1, &v)) return false;
	return (v >> 7) & 0x01u;
}

void codec_refresh_diag(void)
{
	s_cs_page = -1;
	cs_read(CS_PLL_LOCK_STATUS, &s_diag.live_cs_pll);
	cs_read(CS_OSC_SW_STATUS,   &s_diag.live_cs_osc_sw);
	cs_read(CS_HP_CTL,          &s_diag.live_cs_hp_ctl);
	s_tas_page = -1;
	tas_read(1, 46, &s_diag.live_tas_p1r46);
	tas_read(1, 45, &s_diag.live_tas_p1r45);
	tas_read(0, 63, &s_diag.live_tas_p0r63);
	tas_read(0, 64, &s_diag.live_tas_p0r64);
	tas_read(0, 65, &s_diag.live_tas_p0r65);
	tas_read(0, 25, &s_diag.live_tas_p0r25);
}

void codec_note_audio_running(bool running)
{
	s_diag.audio_running = running ? 1 : 0;
}

void codec_note_audio(int cfg_ret, uint8_t stage)
{
	s_diag.audio_i2s_cfg_ret = (int8_t)cfg_ret;
	s_diag.audio_stage = stage;
}

void codec_get_diag(codec_diag_t *out)
{
	if (out) *out = s_diag;
}
