/*
	avr_portmux.h

	"Modern" AVR (AVRxt) Port Multiplexer (PORTMUX at 0x0200 on the tinyAVR
	1-series, also megaAVR-0 and AVR Dx families).

	PORTMUX selects which *physical pins* a peripheral's functions are routed to
	(alternate pin sets for EVOUT/CCL-LUT, USART0, SPI0, TWI0, TCA0 WO0..5, and
	TCB0/1 WO). It has four control registers and no interrupt or state of its
	own.

	simavr connects peripherals to the outside world through their function IRQs
	(e.g. the UART/SPI/TWI wire IRQs), not through numbered physical pins, so the
	choice of pin set has no behavioural effect here. This module therefore models
	PORTMUX as a registered configuration store: the four CTRL registers read back
	what firmware writes and reset to 0 (the default routing), so code that
	configures pin muxing behaves. The write hook is a single place where actual
	pin re-routing could be added later if a peripheral grows PORT-pin outputs.

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

#ifndef __AVR_PORTMUX_H__
#define __AVR_PORTMUX_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "sim_avr.h"

#define AVR_PORTMUX_REGS	4	/* CTRLA..CTRLD */

/* Register offsets within a PORTMUX block (device header PORTMUX_t). */
enum {
	PORTMUXR_CTRLA = 0x00,
	PORTMUXR_CTRLB = 0x01,
	PORTMUXR_CTRLC = 0x02,
	PORTMUXR_CTRLD = 0x03,
};

typedef struct avr_portmux_t {
	avr_io_t	io;
	char		name;

	avr_io_addr_t	base;
} avr_portmux_t;

/*
 * Initialise a PORTMUX block at data address 'base'. 'name' is a tag for debug.
 */
void
avr_portmux_init(
		avr_t * avr,
		avr_portmux_t * p,
		avr_io_addr_t base,
		char name);

#ifdef __cplusplus
};
#endif

#endif /* __AVR_PORTMUX_H__ */
