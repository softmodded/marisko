#include "pwm.h"
#include <soc.h>

/* Track LEDs: P0.29, P0.26, P1.15, P1.14 */
#define PWM_PSEL(port, pin) \
	((PWM_PSEL_OUT_CONNECT_Connected << PWM_PSEL_OUT_CONNECT_Pos) | \
	 ((uint32_t)(port) << PWM_PSEL_OUT_PORT_Pos) | \
	 ((uint32_t)(pin)  << PWM_PSEL_OUT_PIN_Pos))

static uint16_t pwm_duty[4] __attribute__((aligned(4)));

void pwm0_init(void)
{
	NRF_PWM0->PSEL.OUT[0] = PWM_PSEL(0, 29);
	NRF_PWM0->PSEL.OUT[1] = PWM_PSEL(0, 26);
	NRF_PWM0->PSEL.OUT[2] = PWM_PSEL(1, 15);
	NRF_PWM0->PSEL.OUT[3] = PWM_PSEL(1, 14);
	NRF_PWM0->ENABLE     = (PWM_ENABLE_ENABLE_Enabled    << PWM_ENABLE_ENABLE_Pos);
	NRF_PWM0->MODE       = (PWM_MODE_UPDOWN_Up            << PWM_MODE_UPDOWN_Pos);
	NRF_PWM0->PRESCALER  = (PWM_PRESCALER_PRESCALER_DIV_1 << PWM_PRESCALER_PRESCALER_Pos);
	NRF_PWM0->COUNTERTOP = PWM_TOP;
	NRF_PWM0->DECODER    = (PWM_DECODER_LOAD_Individual   << PWM_DECODER_LOAD_Pos) |
	                       (PWM_DECODER_MODE_RefreshCount  << PWM_DECODER_MODE_Pos);
	NRF_PWM0->LOOP   = 0xFFFF;
	NRF_PWM0->SHORTS = (PWM_SHORTS_LOOPSDONE_SEQSTART0_Enabled
	                    << PWM_SHORTS_LOOPSDONE_SEQSTART0_Pos);

	/* 0x8000 = compare=0 with polarity invert → output low (LED off) */
	pwm_duty[0] = pwm_duty[1] = pwm_duty[2] = pwm_duty[3] = 0x8000;

	NRF_PWM0->SEQ[0].PTR      = (uint32_t)pwm_duty;
	NRF_PWM0->SEQ[0].CNT      = 4;
	NRF_PWM0->SEQ[0].REFRESH  = 0;
	NRF_PWM0->SEQ[0].ENDDELAY = 0;
	NRF_PWM0->SEQ[1].PTR      = (uint32_t)pwm_duty;
	NRF_PWM0->SEQ[1].CNT      = 4;
	NRF_PWM0->SEQ[1].REFRESH  = 0;
	NRF_PWM0->SEQ[1].ENDDELAY = 0;
	NRF_PWM0->TASKS_SEQSTART[0] = 1;
}

void pwm0_set_duty(int ch, uint16_t duty)
{
	pwm_duty[ch] = 0x8000u | duty;
}
