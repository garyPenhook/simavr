/*
	attiny3217_adc.c

	Modern-AVR (ATtiny3217) firmware: enable ADC0, select AIN3, run a conversion
	and check the 10-bit result is near half-scale, signalling success on PA3.
	The analog input is provided by the host side.
 */
#include <avr/io.h>
#include "avr_mcu_section.h"

AVR_MCU(20000000, "attiny3217");

int main(void)
{
	PORTA.DIRSET = (1 << 3);
	ADC0.CTRLA = ADC_ENABLE_bm;			// 10-bit, enabled
	ADC0.MUXPOS = 3;					// AIN3
	ADC0.COMMAND = ADC_STCONV_bm;		// start conversion
	while (!(ADC0.INTFLAGS & ADC_RESRDY_bm))
		;
	uint16_t r = ADC0.RES;
	if (r > 500 && r < 524)				// ~512 expected
		PORTA.OUTSET = (1 << 3);
	for (;;)
		;
}
