#pragma once

#include <stdint.h>

#define PWM_TOP 1000

void pwm0_init(void);

/* Set duty for track LED channel 0-3. duty range: 0..PWM_TOP.
 * bit15 of the stored value is the polarity invert flag (0x8000 = active-high). */
void pwm0_set_duty(int ch, uint16_t duty);

/* PWM1 drives the 4 playback LEDs (pb_leds) so they can be dimmed (volume meter).
 * channel i = pb_leds[i]: P1.13, P0.00, P1.12, P0.01. duty 0..PWM_TOP. */
void pwm1_init(void);
void pwm1_set_duty(int ch, uint16_t duty);
