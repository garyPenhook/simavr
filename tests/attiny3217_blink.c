/*
	attiny3217_blink.c

	Minimal modern-AVR (ATtiny3217) firmware for simavr: configure PA3 as an
	output via PORTA.DIRSET and toggle it with PORTA.OUTTGL. Exercises the
	modern PORT peripheral, AVRxt addressing/timing and the attiny3217 core.
	Driven by test_attiny3217_blink.c.
 */
#include <avr/io.h>
#include "avr_mcu_section.h"

AVR_MCU(20000000, "attiny3217");

int main(void)
{
	PORTA.DIRSET = (1 << 3);		// PA3 as output
	for (;;)
		PORTA.OUTTGL = (1 << 3);	// toggle PA3
}
