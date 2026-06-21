#include "leds.h"
#include "util.h"

const struct led pb_leds[NUM_PB_LEDS] = {
	{ NRF_P1, 13 }, { NRF_P0, 0 }, { NRF_P1, 12 }, { NRF_P0, 1 },
};

const struct led track_leds[NUM_TRK_LEDS] = {
	{ NRF_P0, 29 }, { NRF_P0, 26 }, { NRF_P1, 15 }, { NRF_P1, 14 },
};

void leds_init(void)
{
	for (int i = 0; i < NUM_PB_LEDS; i++)
		pb_leds[i].port->PIN_CNF[pb_leds[i].pin] = GPIO_OUT_CNF;
	for (int i = 0; i < NUM_TRK_LEDS; i++)
		track_leds[i].port->PIN_CNF[track_leds[i].pin] = GPIO_OUT_CNF;
	all_pb_off();
	all_trk_off();
}

void all_pb_off(void)
{
	for (int i = 0; i < NUM_PB_LEDS; i++)
		pb_leds[i].port->OUTCLR = (1u << pb_leds[i].pin);
}

void set_pb_on(int i)
{
	pb_leds[i].port->OUTSET = (1u << pb_leds[i].pin);
}

void all_trk_off(void)
{
	for (int i = 0; i < NUM_TRK_LEDS; i++)
		track_leds[i].port->OUTCLR = (1u << track_leds[i].pin);
}

void set_trk_on(int i)
{
	track_leds[i].port->OUTSET = (1u << track_leds[i].pin);
}
