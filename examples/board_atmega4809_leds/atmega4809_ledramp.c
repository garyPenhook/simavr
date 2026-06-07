/*
	atmega4809_ledramp.c

	Firmware for the modern-AVR (megaAVR 0-series) OpenGL LED demo. It drives all
	eight PORTA pins as outputs and bounces a single lit LED back and forth
	("Knight Rider"), at a human-watchable rate, while narrating over USART0.

	This is the firmware side of examples/board_atmega4809_leds, whose host puts
	up a real window showing PORTA as eight LEDs.

	Copyright 2026 simavr authors

 	This file is part of simavr.

	simavr is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	simavr is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with simavr.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <avr/io.h>
#include <util/delay.h>
#include <stdio.h>

// for linker, emulator, and programmer's sake
#include "avr_mcu_section.h"
AVR_MCU(F_CPU, "atmega4809");

#define USART0_BAUD_RATE(br)	((uint16_t)((F_CPU * 64.0) / (16.0 * (br)) + 0.5))

static int
uart_putchar(char c, FILE * stream)
{
	if (c == '\n')
		uart_putchar('\r', stream);
	while (!(USART0.STATUS & USART_DREIF_bm))
		;
	USART0.TXDATAL = c;
	return 0;
}

static FILE mystdout = FDEV_SETUP_STREAM(uart_putchar, NULL, _FDEV_SETUP_WRITE);

int
main(void)
{
	PORTA.DIR = 0xFF;		// all of PORTA = outputs (the 8 LEDs)

	/* USART0 TX on the alternate route (PA4) so it stays off the LED pins. */
	PORTMUX.USARTROUTEA |= PORTMUX_USART0_ALT1_gc;
	USART0.BAUD = USART0_BAUD_RATE(9600);
	USART0.CTRLB = USART_TXEN_bm;
	stdout = &mystdout;

	printf("atmega4809 LED ramp: bouncing one LED across PORTA\n");

	uint8_t pos = 0;
	int8_t  dir = 1;
	for (;;) {
		PORTA.OUT = (uint8_t)(1u << pos);
		printf("LED %u\n", pos);
		_delay_ms(120);				// watchable step rate

		pos = (uint8_t)(pos + dir);
		if (pos == 7)
			dir = -1;
		else if (pos == 0)
			dir = 1;
	}
}
