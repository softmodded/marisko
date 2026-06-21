#pragma once

#include <stdint.h>

#define PWM_TOP 1000

void pwm0_init(void);

/* Set duty for track LED channel 0-3. duty range: 0..PWM_TOP.
 * bit15 of the stored value is the polarity invert flag (0x8000 = active-high). */
void pwm0_set_duty(int ch, uint16_t duty);
