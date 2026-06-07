#include <zephyr/kernel.h>
#include <soc.h>

#define NRF_P0_OUT (*(volatile uint32_t *)0x50000504)
#define NRF_P1_OUT (*(volatile uint32_t *)0x50000804)

struct led { NRF_GPIO_Type *port; uint32_t pin; };
static const struct led leds[] = {
	{ NRF_P1, 13 }, { NRF_P0, 0 }, { NRF_P1, 12 }, { NRF_P0, 1 },
};
#define NUM_LEDS (sizeof(leds) / sizeof(leds[0]))

static void all_off(void)
{
	for (int i = 0; i < NUM_LEDS; i++)
		leds[i].port->OUTCLR = (1 << leds[i].pin);
}

static void set_on(int i)
{
	leds[i].port->OUTSET = (1 << leds[i].pin);
}

static void feed_wdt(void)
{
	for (int ch = 0; ch < 8; ch++)
		NRF_WDT->RR[ch] = WDT_RR_RR_Reload;
}

static void delay_50ms(void)
{
	for (volatile uint32_t d = 0; d < 600000; d++)
		__ASM volatile ("nop");
}

int main(void)
{
	NRF_P0->PIN_CNF[27] = (GPIO_PIN_CNF_DIR_Input    << GPIO_PIN_CNF_DIR_Pos)   |
			       (GPIO_PIN_CNF_PULL_Pullup << GPIO_PIN_CNF_PULL_Pos) |
			       (GPIO_PIN_CNF_INPUT_Connect << GPIO_PIN_CNF_INPUT_Pos);

	for (int i = 0; i < NUM_LEDS; i++)
		leds[i].port->PIN_CNF[leds[i].pin] =
			(GPIO_PIN_CNF_DIR_Output   << GPIO_PIN_CNF_DIR_Pos)   |
			(GPIO_PIN_CNF_DRIVE_S0S1 << GPIO_PIN_CNF_DRIVE_Pos) |
			(GPIO_PIN_CNF_INPUT_Connect << GPIO_PIN_CNF_INPUT_Pos);

	all_off();

	int pos = 0, dir = 1;

	while (1) {
		if (!(NRF_P0->IN & (1 << 27))) {
			all_off();
			feed_wdt();
			NRF_POWER->RESETREAS = 0xFFFFFFFF;
			NRF_POWER->SYSTEMOFF = 1;
			__DSB();
			for (;;);
		}

		all_off();
		set_on(pos);
		*(volatile uint32_t *)0x2000FFF0 = (NRF_P0->OUT & 0xFFFF) | ((NRF_P1->OUT & 0xFFFF) << 16);
		delay_50ms();
		delay_50ms();
		feed_wdt();

		pos += dir;
		if (pos == NUM_LEDS - 1) dir = -1;
		if (pos == 0)             dir = 1;

		if (!(NRF_P0->IN & (1 << 27))) {
			all_off();
			feed_wdt();
			NRF_POWER->RESETREAS = 0xFFFFFFFF;
			NRF_POWER->SYSTEMOFF = 1;
			__DSB();
			for (;;);
		}
	}

	return 0;
}
