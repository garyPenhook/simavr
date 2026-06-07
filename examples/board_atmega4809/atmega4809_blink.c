/*
	atmega4809_blink.c

	A minimal "modern AVR" (AVRxt / megaAVR 0-series) firmware demo for simavr.
	It blinks PA0 using the modern PORT register block (DIRSET / OUTTGL) and
	emits a greeting plus a running counter over USART0, exercising the
	avr_port_modern and avr_usart_modern peripheral models.

	Unlike the classic-AVR examples in this tree, this targets the modern
	register layout: peripherals are memory-mapped structs (PORTA, USART0)
	from the device header rather than the flat <avr/io.h> register macros.

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

/*
 * USART0 on the megaAVR-0 lives at PA0/PA1 by default, but PA0 is our LED here,
 * so route TXD over the alternate PORTMUX pins (PA4) and just drive TX.
 * 9600 baud is plenty for a demo and keeps the BAUD register value small.
 */
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

static void
usart0_init(void)
{
	PORTMUX.USARTROUTEA |= PORTMUX_USART0_ALT1_gc;	// TXD/RXD -> PA4/PA5
	PORTA.DIRSET = PIN4_bm;							// PA4 = TXD output
	USART0.BAUD = USART0_BAUD_RATE(9600);
	USART0.CTRLB = USART_TXEN_bm;
}

int
main(void)
{
	PORTA.DIRSET = PIN0_bm;		// PA0 as output (the "LED")

	usart0_init();
	stdout = &mystdout;

	printf("atmega4809 modern-AVR blink demo\n");

	uint16_t n = 0;
	for (;;) {
		PORTA.OUTTGL = PIN0_bm;	// toggle PA0
		printf("tick %u\n", n++);
		_delay_ms(10);
	}
}
