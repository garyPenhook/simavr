/*
	attiny3217_blink.c

	A minimal "blink an LED on PA0" firmware for the modern-AVR (AVRxt)
	ATtiny3217 core added by this fork. Build it with a modern avr-gcc
	(one that supports -mmcu=attiny3217) and run it on simavr's run_avr, or
	load it from the host demo tests/test_attiny3217_blink.c.

	The AVR_MCU* tags below let run_avr discover the part and auto-trace PA0
	to a VCD without any command-line flags:

	    make attiny3217-demo          # build + run the host demo
	    # or, directly:
	    avr-gcc -mmcu=attiny3217 -Os -o blink.axf attiny3217_blink.c
	    ../simavr/run_avr blink.axf   # writes attiny3217_blink.vcd

	Copyright 2026 simavr authors. GNU GPL v3 or later; see COPYING.
 */

#include <avr/io.h>
#include <stdint.h>
#include "avr_mcu_section.h"

AVR_MCU(F_CPU, "attiny3217");
AVR_MCU_VCD_FILE("attiny3217_blink.vcd", 1000 /* us per sample */);
AVR_MCU_VCD_PORT_PIN('A', 0, "PA0");

int
main(void)
{
	PORTA.DIRSET = PIN0_bm;			/* PA0 = output */
	for (;;) {
		PORTA.OUTTGL = PIN0_bm;		/* toggle the LED on PA0 */
		for (volatile uint16_t i = 0; i < 4000; i++)
			;			/* crude delay between toggles */
	}
	return 0;
}
