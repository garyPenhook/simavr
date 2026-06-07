/*
	blink_fw.c

	A *device-agnostic* modern-AVR (AVRxt) blink firmware. The same source is
	compiled once per supported modern core (all 15 tinyAVR 1-series + 8
	megaAVR 0-series) by this board's Makefile, each build passing the device
	name in MCU_NAME so the embedded .mmcu section names the right core for
	simavr.

	It touches only the peripherals that exist identically on every modern AVR,
	from the 8-pin ATtiny212 up to the 48-pin ATmega4809:

	  * PORTA / PA0  — the one port+pin present on every part (blink)
	  * USART0       — present on every part at the same base (0x0800), TX byte
	                   per loop. The modern USART model emits on the wire IRQ the
	                   instant TXDATAL is written, independent of PORTMUX / pin
	                   direction, so this needs no per-device pin knowledge.

	Anything that varies by device (which physical TXD pin, extra ports/USARTs)
	is intentionally left out so one source builds and runs unchanged across the
	whole family; see board_atmega4809 / board_attiny3217 for richer per-family
	demos that drive the real pins.

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

// for linker, emulator, and programmer's sake
#include "avr_mcu_section.h"

#ifndef MCU_NAME
#error "MCU_NAME must be defined by the build (e.g. -DMCU_NAME='\"attiny3217\"')"
#endif

AVR_MCU(F_CPU, MCU_NAME);

/* 9600 baud; the exact value is irrelevant to the wire IRQ, only that TX runs. */
#define USART0_BAUD_RATE(br)	((uint16_t)((F_CPU * 64.0) / (16.0 * (br)) + 0.5))

int
main(void)
{
	PORTA.DIRSET = PIN0_bm;		// PA0 as output (the "LED")

	/* Bring up USART0 TX. No PORTMUX / DIR: the model emits on TXDATAL write. */
	USART0.BAUD = USART0_BAUD_RATE(9600);
	USART0.CTRLB = USART_TXEN_bm;

	uint8_t n = 0;
	for (;;) {
		PORTA.OUTTGL = PIN0_bm;	// toggle PA0

		while (!(USART0.STATUS & USART_DREIF_bm))
			;
		USART0.TXDATAL = n++;	// emit a byte on the USART0 wire IRQ

		_delay_ms(10);
	}
}
