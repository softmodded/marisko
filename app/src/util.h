#pragma once

#include <zephyr/kernel.h>
#include <soc.h>
#include <stdint.h>

/* Standard GPIO output config: push-pull, input connected */
#define GPIO_OUT_CNF \
	((GPIO_PIN_CNF_DIR_Output    << GPIO_PIN_CNF_DIR_Pos)   | \
	 (GPIO_PIN_CNF_DRIVE_S0S1   << GPIO_PIN_CNF_DRIVE_Pos)  | \
	 (GPIO_PIN_CNF_INPUT_Connect << GPIO_PIN_CNF_INPUT_Pos))

static inline void feed_wdt(void)
{
	for (int ch = 0; ch < 8; ch++)
		NRF_WDT->RR[ch] = WDT_RR_RR_Reload;
}

/* ~1 ms per count at 64 MHz (12000 NOPs). Not precise — use only for init delays. */
static inline void delay_ms(uint32_t ms)
{
	for (volatile uint32_t d = 0; d < ms * 12000; d++)
		__ASM volatile ("nop");
}
