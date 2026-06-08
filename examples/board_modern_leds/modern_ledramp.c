/*
	modern_ledramp.c

	A *device-agnostic* modern-AVR (AVRxt) LED-ramp firmware for the visual board
	examples/board_modern_leds. The same source is compiled once per supported
	modern core (all 15 tinyAVR 1-series + 8 megaAVR 0-series) by this board's
	Makefile, each build passing the device name in MCU_NAME so the embedded
	.mmcu section names the right core for simavr.

	It drives all eight PORTA pins as outputs and bounces a single lit LED back
	and forth ("Knight Rider") at a human-watchable rate, narrating the position
	over USART0. Everything it touches — PORTA and USART0 — exists identically on
	every modern AVR, so one source builds and runs unchanged from the 8-pin
	ATtiny212 up to the 48-pin ATmega4809.

	The trick that keeps it device-agnostic: USART0 TX is enabled WITHOUT routing
	a physical TXD pin (no DIRSET, no PORTMUX). The default TXD pin differs by
	family (PB2 on tinyAVR-1, PA0 on megaAVR-0), but simavr captures USART output
	through the UART IRQ mesh independent of the physical pin, so the host sees
	the narration on every core and PORTA stays entirely free for the 8 LEDs.

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

#ifndef MCU_NAME
#error "MCU_NAME must be defined by the build (e.g. -DMCU_NAME='\"attiny3217\"')"
#endif

AVR_MCU(F_CPU, MCU_NAME);

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

	/*
	 * USART0 TX as narration transport. No TXD pin / PORTMUX setup: the default
	 * route differs by family, but simavr captures TX via the UART IRQ mesh
	 * regardless, keeping this source identical across all 23 modern cores and
	 * leaving every PORTA pin free for the LEDs.
	 */
	USART0.BAUD = USART0_BAUD_RATE(9600);
	USART0.CTRLB = USART_TXEN_bm;
	stdout = &mystdout;

	printf("%s LED ramp: bouncing one LED across PORTA\n", MCU_NAME);

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
