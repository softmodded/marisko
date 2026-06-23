#include "saadc.h"
#include <soc.h>
#include <zephyr/kernel.h>

/* SAADC is a single shared peripheral reconfigured per read, so concurrent
 * callers (main thread ladders + the high-priority fader thread) must not
 * overlap. Serialize every read. */
K_MUTEX_DEFINE(s_saadc_lock);

#define SAADC_ERRATA236() \
	(*((volatile uint32_t *)((uint8_t *)NRF_SAADC + 0x63C)) = 0x10000000UL)

static int16_t adc_result __attribute__((aligned(4)));

static void saadc_reset(void)
{
	NRF_SAADC->ENABLE = 0;
	SAADC_ERRATA236();
	NRF_SAADC->ENABLE = (SAADC_ENABLE_ENABLE_Enabled << SAADC_ENABLE_ENABLE_Pos);
}

void saadc_init(void)
{
	/* Bypass oversampling while DISABLED — the SAADC latches OVERSAMPLE at
	 * enable, so setting it per-read (while enabled) doesn't take. The bootloader
	 * leaves it high (~128×), making every read ~1.3 ms; bypassed it's ~20 µs,
	 * cheap enough to poll from the high-priority UI thread without starving the
	 * audio feed. */
	NRF_SAADC->ENABLE = 0;
	NRF_SAADC->OVERSAMPLE = (SAADC_OVERSAMPLE_OVERSAMPLE_Bypass << SAADC_OVERSAMPLE_OVERSAMPLE_Pos);
	SAADC_ERRATA236();
	NRF_SAADC->ENABLE = (SAADC_ENABLE_ENABLE_Enabled << SAADC_ENABLE_ENABLE_Pos);
}

static int saadc_read_locked(uint32_t pselp)
{
	NRF_SAADC->CH[0].PSELP  = (pselp                       << SAADC_CH_PSELP_PSELP_Pos);
	NRF_SAADC->CH[0].PSELN  = (SAADC_CH_PSELN_PSELN_NC     << SAADC_CH_PSELN_PSELN_Pos);
	NRF_SAADC->CH[0].CONFIG =
		(SAADC_CH_CONFIG_REFSEL_Internal << SAADC_CH_CONFIG_REFSEL_Pos) |
		(SAADC_CH_CONFIG_GAIN_Gain1_6    << SAADC_CH_CONFIG_GAIN_Pos)   |
		(SAADC_CH_CONFIG_TACQ_10us       << SAADC_CH_CONFIG_TACQ_Pos)   |
		(SAADC_CH_CONFIG_MODE_SE         << SAADC_CH_CONFIG_MODE_Pos)   |
		(SAADC_CH_CONFIG_BURST_Enabled   << SAADC_CH_CONFIG_BURST_Pos);
	NRF_SAADC->RESOLUTION    = (SAADC_RESOLUTION_VAL_12bit << SAADC_RESOLUTION_VAL_Pos);
	/* Bypass oversampling. The bootloader leaves OVERSAMPLE set high, and with
	 * BURST that makes every single read ~128× oversample (~1.75 ms) — far too
	 * slow when polled from the high-priority UI thread (it starved the audio
	 * feed → underruns). A single conversion is ~20 µs. */
	NRF_SAADC->OVERSAMPLE    = (SAADC_OVERSAMPLE_OVERSAMPLE_Bypass << SAADC_OVERSAMPLE_OVERSAMPLE_Pos);
	NRF_SAADC->RESULT.PTR    = (uint32_t)&adc_result;
	NRF_SAADC->RESULT.MAXCNT = 1;
	adc_result = 0;

	/* Spin caps bound how long this poll busy-waits if a SAADC event never fires
	 * (errata/idle). A real 12-bit/TACQ=10µs conversion completes in <200 of
	 * these (volatile) iterations, so 4000 is 20× margin yet caps a FAILED read
	 * to <1 ms — important because the UI thread polls faders at high priority,
	 * where a multi-ms spin would starve the audio feed thread → underruns. */
	NRF_SAADC->EVENTS_STARTED = 0;
	NRF_SAADC->TASKS_START    = 1;
	for (volatile int t = 0; t < 4000; t++)
		if (NRF_SAADC->EVENTS_STARTED) break;
	if (!NRF_SAADC->EVENTS_STARTED) { saadc_reset(); return -1; }
	NRF_SAADC->EVENTS_STARTED = 0;

	NRF_SAADC->EVENTS_END   = 0;
	NRF_SAADC->TASKS_SAMPLE = 1;
	__DSB();
	for (volatile int t = 0; t < 4000; t++)
		if (NRF_SAADC->EVENTS_END) break;
	if (!NRF_SAADC->EVENTS_END) { saadc_reset(); return -1; }
	NRF_SAADC->EVENTS_END = 0;

	NRF_SAADC->EVENTS_STOPPED = 0;
	NRF_SAADC->TASKS_STOP     = 1;
	for (volatile int t = 0; t < 2000; t++)
		if (NRF_SAADC->EVENTS_STOPPED) break;
	NRF_SAADC->EVENTS_STOPPED = 0;

	return (adc_result < 0) ? 0 : (int)adc_result;
}

int saadc_read(uint32_t pselp)
{
	k_mutex_lock(&s_saadc_lock, K_FOREVER);
	int r = saadc_read_locked(pselp);
	k_mutex_unlock(&s_saadc_lock);
	return r;
}
