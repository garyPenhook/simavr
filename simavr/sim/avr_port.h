/*
	avr_port.h

	Modern AVR (AVRxt: tinyAVR 0/1-series, megaAVR-0, AVR Dx) I/O port (PORT).
	This is the modern equivalent of avr_ioport.c. A modern PORT is a register
	block (PORTx) with DIR/OUT plus SET/CLR/TGL convenience registers, an IN
	register and per-pin control. Each pin level is exposed as an IRQ, mirroring
	avr_ioport so boards/tests can observe and drive pins.

	Copyright 2024 simavr modern-AVR fork (garyPenhook)

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

#ifndef __AVR_PORT_H__
#define __AVR_PORT_H__

#include "sim_avr.h"
#include "sim_interrupts.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
	AVR_PORT_IRQ_PIN0 = 0,
	AVR_PORT_IRQ_PIN1, AVR_PORT_IRQ_PIN2, AVR_PORT_IRQ_PIN3,
	AVR_PORT_IRQ_PIN4, AVR_PORT_IRQ_PIN5, AVR_PORT_IRQ_PIN6, AVR_PORT_IRQ_PIN7,
	AVR_PORT_IRQ_PIN_ALL,	// all 8 pins as one 8-bit value
	AVR_PORT_IRQ_COUNT
};

// Register offsets from the PORTx base address (data space).
enum {
	AVR_PORT_DIR = 0, AVR_PORT_DIRSET, AVR_PORT_DIRCLR, AVR_PORT_DIRTGL,
	AVR_PORT_OUT, AVR_PORT_OUTSET, AVR_PORT_OUTCLR, AVR_PORT_OUTTGL,
	AVR_PORT_IN, AVR_PORT_INTFLAGS,
	AVR_PORT_PIN0CTRL = 0x10,
};

typedef struct avr_port_t {
	avr_io_t			io;
	char				name;		// 'A', 'B', 'C' ...
	avr_io_addr_t		r_base;		// data address of PORTx (the DIR register)
	avr_int_vector_t	port_vect;	// PORTx_PORT pin-change interrupt (optional)

	// default values driven onto pins configured as inputs (set via ioctl)
	uint8_t				external_mask, external_value;
	uint8_t				irqing;		// suppress pin-IRQ feedback during update
} avr_port_t;

void avr_port_init(avr_t * avr, avr_port_t * port);

// add the port name (uppercase letter) to get its IRQ base
#define AVR_IOCTL_PORT_GETIRQ(_name) AVR_IOCTL_DEF('p','r','t',(_name))

#ifdef __cplusplus
};
#endif

#endif /* __AVR_PORT_H__ */
