#include "saadc.h"
#include <soc.h>

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
	SAADC_ERRATA236();
	NRF_SAADC->ENABLE = (SAADC_ENABLE_ENABLE_Enabled << SAADC_ENABLE_ENABLE_Pos);
}

int saadc_read(uint32_t pselp)
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
	NRF_SAADC->RESULT.PTR    = (uint32_t)&adc_result;
	NRF_SAADC->RESULT.MAXCNT = 1;
	adc_result = 0;

	/* Spin caps bound how long this prio-0 poll can busy-wait when a SAADC event
	 * never fires (errata/idle): worst case was ~34 ms/read × 2 ladders = ~68 ms,
	 * exceeding the I2S feed thread's ~61 ms TX runway → underrun → audio cuts.
	 * A real 12-bit/TACQ=10µs conversion completes in <2000 iters, so 50000 is
	 * 25× margin yet caps a failed read to ~3 ms. */
	NRF_SAADC->EVENTS_STARTED = 0;
	NRF_SAADC->TASKS_START    = 1;
	for (volatile int t = 0; t < 50000; t++)
		if (NRF_SAADC->EVENTS_STARTED) break;
	if (!NRF_SAADC->EVENTS_STARTED) { saadc_reset(); return -1; }
	NRF_SAADC->EVENTS_STARTED = 0;

	NRF_SAADC->EVENTS_END   = 0;
	NRF_SAADC->TASKS_SAMPLE = 1;
	__DSB();
	for (volatile int t = 0; t < 50000; t++)
		if (NRF_SAADC->EVENTS_END) break;
	if (!NRF_SAADC->EVENTS_END) { saadc_reset(); return -1; }
	NRF_SAADC->EVENTS_END = 0;

	NRF_SAADC->EVENTS_STOPPED = 0;
	NRF_SAADC->TASKS_STOP     = 1;
	for (volatile int t = 0; t < 10000; t++)
		if (NRF_SAADC->EVENTS_STOPPED) break;
	NRF_SAADC->EVENTS_STOPPED = 0;

	return (adc_result < 0) ? 0 : (int)adc_result;
}
