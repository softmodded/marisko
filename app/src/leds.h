#pragma once

#include <soc.h>
#include <stdint.h>

struct led { NRF_GPIO_Type *port; uint32_t pin; };

#define NUM_PB_LEDS  4
#define NUM_TRK_LEDS 4

extern const struct led pb_leds[NUM_PB_LEDS];
extern const struct led track_leds[NUM_TRK_LEDS];

void leds_init(void);

void all_pb_off(void);
void set_pb_on(int i);

void all_trk_off(void);
void set_trk_on(int i);
