/*
	attiny3217_tca.c

	Modern-AVR (ATtiny3217) firmware: TCA0 in Normal mode with an overflow
	interrupt (toggles PA3) and a CMP0 compare interrupt (toggles PA4). Exercises
	the TCA counter, prescaler, overflow and compare interrupts via CPUINT.
 */
#include <avr/io.h>
#include <avr/interrupt.h>
#include "avr_mcu_section.h"

AVR_MCU(20000000, "attiny3217");

ISR(TCA0_OVF_vect)
{
	TCA0.SINGLE.INTFLAGS = TCA_SINGLE_OVF_bm;
	PORTA.OUTTGL = (1 << 3);
}

ISR(TCA0_CMP0_vect)
{
	TCA0.SINGLE.INTFLAGS = TCA_SINGLE_CMP0_bm;
	PORTA.OUTTGL = (1 << 4);
}

int main(void)
{
	PORTA.DIRSET = (1 << 3) | (1 << 4);
	TCA0.SINGLE.PER = 1000;
	TCA0.SINGLE.CMP0 = 400;
	TCA0.SINGLE.INTCTRL = TCA_SINGLE_OVF_bm | TCA_SINGLE_CMP0_bm;
	TCA0.SINGLE.CTRLA = TCA_SINGLE_CLKSEL_DIV1_gc | TCA_SINGLE_ENABLE_bm;
	sei();
	for (;;)
		;
}
