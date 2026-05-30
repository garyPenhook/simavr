/*
	attiny3217_tcb_irq.c

	Modern-AVR (ATtiny3217) firmware: TCB0 periodic interrupt toggles PA3 from
	its ISR. Exercises the CPUINT interrupt controller, the TCB timer and the
	PORT peripheral together. Driven by test_attiny3217_tcb_irq.c.
 */
#include <avr/io.h>
#include <avr/interrupt.h>
#include "avr_mcu_section.h"

AVR_MCU(20000000, "attiny3217");

volatile uint16_t ticks;

ISR(TCB0_INT_vect)
{
	TCB0.INTFLAGS = TCB_CAPT_bm;	// clear the (sticky) flag
	ticks++;
	PORTA.OUTTGL = (1 << 3);		// observable from the host side
}

int main(void)
{
	PORTA.DIRSET = (1 << 3);		// PA3 output
	TCB0.CCMP = 500;
	TCB0.INTCTRL = TCB_CAPT_bm;		// enable capture interrupt
	TCB0.CTRLA = TCB_ENABLE_bm;		// CLKSEL=CLK_PER/1, CNTMODE=periodic
	sei();
	for (;;)
		;
}
