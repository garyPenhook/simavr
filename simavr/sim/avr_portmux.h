/*
	avr_portmux.h

	"Modern" AVR (AVRxt) Port Multiplexer (PORTMUX at 0x0200 on the tinyAVR
	1-series, also megaAVR-0 and AVR Dx families).

	PORTMUX selects which *physical pins* a peripheral's functions are routed to
	(alternate pin sets for EVOUT/CCL-LUT, USART0, SPI0, TWI0, TCA0 WO0..5, and
	TCB0/1 WO). It has four control registers and no interrupt or state of its
	own.

	Peripherals mostly reach the outside world through their own function IRQs
	(UART/SPI/TWI wire IRQs). On top of that, a peripheral output can be
	*routed*: registered with avr_portmux_add_func() and bound to a pin-state IRQ,
	it overrides the PORT pin its routing field selects (avr_port_modern_override)
	and moves when the field is written. Only devices that opt in via a
	SIM_PORTMUX_* table in their core file route anything (so far the ATtiny3216/
	3217: TCA0 WO0-2 and USART0 TXD); everywhere else the registers just read
	back what firmware writes and reset to 0 (the default routing).

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
#include "avr_port_modern.h"

#define AVR_PORTMUX_REGS	4	/* CTRLA..CTRLD */
#define AVR_PORTMUX_REGS_MAX	10	/* AVR DD: EVSYSROUTEA..TCDROUTEA (offsets 0-9) */

/* Register offsets within a PORTMUX block (device header PORTMUX_t). */
enum {
	PORTMUXR_CTRLA = 0x00,
	PORTMUXR_CTRLB = 0x01,
	PORTMUXR_CTRLC = 0x02,
	PORTMUXR_CTRLD = 0x03,
};

#define AVR_PORTMUX_MAX_FUNCS	32	/* routed peripheral output functions */
#define AVR_PORTMUX_MAX_OPTS	8	/* pin choices per routing field */

/* A physical pin: PORT letter ('A'..) and bit 0-7. port == 0 means "no pin". */
typedef struct avr_portmux_pin_t {
	char		port;
	int8_t		pin;
} avr_portmux_pin_t;

/*
 * One routed peripheral output (a TCA waveform output, a USART TXD ...).
 * The routing register field (reg/mask/shift) selects which of 'pins' it
 * appears on. While the peripheral marks it active it overrides that pin's
 * output level (avr_port_modern_override); changing the field, or going
 * inactive, releases the old pin. A pin choice of port 0 (e.g. a field value
 * the package lacks) routes nowhere.
 */
typedef struct avr_portmux_func_t {
	uint8_t		reg, mask, shift;
	avr_portmux_pin_t	pins[AVR_PORTMUX_MAX_OPTS];
	uint8_t		force_output;	/* peripheral also forces the pin to an output */

	uint8_t		active, value;	/* set by the peripheral */
	avr_portmux_pin_t	claimed;	/* pin currently overridden, if any */

	struct avr_portmux_t *pm;	/* back-pointer, for IRQ binding */
	int		index;
} avr_portmux_func_t;

typedef struct avr_portmux_t {
	avr_io_t	io;
	char		name;

	avr_io_addr_t	base;
	uint8_t		nregs;

	avr_port_modern_t	*ports['Z' - 'A' + 1];	/* by PORT letter */
	avr_portmux_func_t	funcs[AVR_PORTMUX_MAX_FUNCS];
	int		nfuncs;
} avr_portmux_t;

/* Tell PORTMUX which PORT instance is letter 'port' (call once per fitted port). */
void
avr_portmux_set_port(
		avr_portmux_t * p,
		char port,
		avr_port_modern_t * pt);

/*
 * Register a routed output; returns its handle (or -1 if the table is full).
 * 'pins' lists the pin for each value of the routing field, starting at 0.
 */
int
avr_portmux_add_func(
		avr_portmux_t * p,
		uint8_t reg,
		uint8_t mask,
		uint8_t shift,
		const avr_portmux_pin_t * pins,
		int npins,
		int force_output);

/*
 * Bind a peripheral "pin state" IRQ to a function: bit 0 of the IRQ value is the
 * level, bit 1 says the peripheral overrides the pin (as AVR_TCA_IRQ_PINn).
 */
void
avr_portmux_bind_irq(
		avr_portmux_t * p,
		int func,
		struct avr_irq_t * irq);

/* The peripheral owns (active) or releases its pin and drives 'value'. */
void
avr_portmux_func_set(
		avr_portmux_t * p,
		int func,
		int active,
		int value);

/*
 * Initialise a PORTMUX block at data address 'base'. 'name' is a tag for debug.
 */
void
avr_portmux_init(
		avr_t * avr,
		avr_portmux_t * p,
		avr_io_addr_t base,
		char name);

/*
 * As avr_portmux_init() but with 'nregs' routing registers: the tinyAVR
 * 2-series block is six *ROUTEA registers (DS40002311B 16.2), not CTRLA..CTRLD.
 */
void
avr_portmux_init_n(
		avr_t * avr,
		avr_portmux_t * p,
		avr_io_addr_t base,
		uint8_t nregs,
		char name);

#ifdef __cplusplus
};
#endif

#endif /* __AVR_PORTMUX_H__ */
