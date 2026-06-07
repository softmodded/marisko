#include <zephyr/kernel.h>
#include <soc.h>

struct led { NRF_GPIO_Type *port; uint32_t pin; };
static const struct led pb_leds[] = {
	{ NRF_P1, 13 }, { NRF_P0, 0 }, { NRF_P1, 12 }, { NRF_P0, 1 },
};

static const struct led track_leds[] = {
	{ NRF_P0, 29 }, { NRF_P0, 26 }, { NRF_P1, 15 }, { NRF_P1, 14 },
};

#define NUM_PB_LEDS   (sizeof(pb_leds) / sizeof(pb_leds[0]))
#define NUM_TRK_LEDS  (sizeof(track_leds) / sizeof(track_leds[0]))

static void all_pb_off(void)
{
	for (int i = 0; i < NUM_PB_LEDS; i++)
		pb_leds[i].port->OUTCLR = (1 << pb_leds[i].pin);
}

static void set_pb_on(int i)
{
	pb_leds[i].port->OUTSET = (1 << pb_leds[i].pin);
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

static int read_ladder(void)
{
	NRF_SAADC->ENABLE = SAADC_ENABLE_ENABLE_Enabled << SAADC_ENABLE_ENABLE_Pos;

	NRF_SAADC->RESOLUTION = SAADC_RESOLUTION_VAL_12bit << SAADC_RESOLUTION_VAL_Pos;

	NRF_SAADC->CH[0].PSELP = SAADC_CH_PSELP_PSELP_AnalogInput0 << SAADC_CH_PSELP_PSELP_Pos;
	NRF_SAADC->CH[0].PSELN = SAADC_CH_PSELN_PSELN_NC << SAADC_CH_PSELN_PSELN_Pos;
	NRF_SAADC->CH[0].CONFIG =
		(SAADC_CH_CONFIG_REFSEL_Internal << SAADC_CH_CONFIG_REFSEL_Pos) |
		(SAADC_CH_CONFIG_GAIN_Gain1_6 << SAADC_CH_CONFIG_GAIN_Pos) |
		(SAADC_CH_CONFIG_TACQ_10us << SAADC_CH_CONFIG_TACQ_Pos) |
		(SAADC_CH_CONFIG_MODE_SE << SAADC_CH_CONFIG_MODE_Pos);

	int16_t result = 0;
	NRF_SAADC->RESULT.PTR = (uint32_t)&result;
	NRF_SAADC->RESULT.MAXCNT = 1;

	NRF_SAADC->TASKS_START = 1;
	for (volatile int timeout = 0; timeout < 50000; timeout++) {
		if (NRF_SAADC->EVENTS_END)
			break;
	}
	if (!NRF_SAADC->EVENTS_END) {
		NRF_SAADC->ENABLE = SAADC_ENABLE_ENABLE_Disabled << SAADC_ENABLE_ENABLE_Pos;
		return -1;
	}
	NRF_SAADC->EVENTS_END = 0;

	NRF_SAADC->TASKS_STOP = 1;
	for (volatile int timeout = 0; timeout < 50000; timeout++) {
		if (NRF_SAADC->EVENTS_STOPPED)
			break;
	}
	NRF_SAADC->EVENTS_STOPPED = 0;

	NRF_SAADC->ENABLE = SAADC_ENABLE_ENABLE_Disabled << SAADC_ENABLE_ENABLE_Pos;

	if (result < 0) result = 0;
	return result;
}

int main(void)
{
	NRF_P0->PIN_CNF[27] = (GPIO_PIN_CNF_DIR_Input    << GPIO_PIN_CNF_DIR_Pos)   |
			       (GPIO_PIN_CNF_PULL_Pullup << GPIO_PIN_CNF_PULL_Pos) |
			       (GPIO_PIN_CNF_INPUT_Connect << GPIO_PIN_CNF_INPUT_Pos);

	for (int i = 0; i < NUM_PB_LEDS; i++)
		pb_leds[i].port->PIN_CNF[pb_leds[i].pin] =
			(GPIO_PIN_CNF_DIR_Output   << GPIO_PIN_CNF_DIR_Pos)   |
			(GPIO_PIN_CNF_DRIVE_S0S1 << GPIO_PIN_CNF_DRIVE_Pos) |
			(GPIO_PIN_CNF_INPUT_Connect << GPIO_PIN_CNF_INPUT_Pos);

	for (int i = 0; i < NUM_TRK_LEDS; i++)
		track_leds[i].port->PIN_CNF[track_leds[i].pin] =
			(GPIO_PIN_CNF_DIR_Output   << GPIO_PIN_CNF_DIR_Pos)   |
			(GPIO_PIN_CNF_DRIVE_S0S1 << GPIO_PIN_CNF_DRIVE_Pos) |
			(GPIO_PIN_CNF_INPUT_Connect << GPIO_PIN_CNF_INPUT_Pos);

	all_pb_off();
	for (int i = 0; i < NUM_TRK_LEDS; i++)
		track_leds[i].port->OUTCLR = (1 << track_leds[i].pin);

	int pos = 0, dir = 1;

	while (1) {
		if (!(NRF_P0->IN & (1 << 27))) {
			all_pb_off();
			for (int i = 0; i < NUM_TRK_LEDS; i++)
				track_leds[i].port->OUTCLR = (1 << track_leds[i].pin);
			feed_wdt();
			NRF_POWER->RESETREAS = 0xFFFFFFFF;
			NRF_POWER->SYSTEMOFF = 1;
			__DSB();
			for (;;);
		}

		int ladder = read_ladder();
		int pressed = 0;
		if (ladder < 100)      pressed = 0;
		else if (ladder < 800)  pressed = 1;
		else if (ladder < 1500) pressed = 2;
		else if (ladder < 2200) pressed = 3;
		else if (ladder < 3000) pressed = 4;

		for (int i = 0; i < NUM_TRK_LEDS; i++) {
			if (pressed == i + 1)
				track_leds[i].port->OUTSET = (1 << track_leds[i].pin);
			else
				track_leds[i].port->OUTCLR = (1 << track_leds[i].pin);
		}

		all_pb_off();
		if (!pressed) set_pb_on(pos);

		*(volatile uint32_t *)0x2000FFF0 = NRF_P0->OUT;
		*(volatile uint32_t *)0x2000FFF4 = NRF_P1->OUT;
		delay_50ms();
		delay_50ms();
		feed_wdt();

		if (!pressed) {
			pos += dir;
			if (pos == NUM_PB_LEDS - 1) dir = -1;
			if (pos == 0)               dir = 1;
		}

		if (!(NRF_P0->IN & (1 << 27))) {
			all_pb_off();
			for (int i = 0; i < NUM_TRK_LEDS; i++)
				track_leds[i].port->OUTCLR = (1 << track_leds[i].pin);
			feed_wdt();
			NRF_POWER->RESETREAS = 0xFFFFFFFF;
			NRF_POWER->SYSTEMOFF = 1;
			__DSB();
			for (;;);
		}
	}

	return 0;
}
