/*
	avr_port_modern.h

	"Modern" AVR (AVRxt) PORT / VPORT peripheral, as found on the tinyAVR
	1-series (ATtiny3217), megaAVR-0 and AVR Dx families.

	Each PORT is a register block (DIR/OUT/IN with SET/CLR/TGL aliases, per-pin
	PINnCTRL, and INTFLAGS pin interrupts). The matching VPORT is a 4-byte,
	bit-addressable alias (DIR/OUT/IN/INTFLAGS) living in the low I/O space so it
	is reachable with SBI/CBI. This module reuses the classic IOPORT IRQ / ioctl
	abstraction (avr_ioport.h) so existing simavr parts (LEDs, buttons, …) and
	VCD wiring connect to it unchanged.

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

#ifndef __AVR_PORT_MODERN_H__
#define __AVR_PORT_MODERN_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "sim_avr.h"
#include "avr_ioport.h"		// reuse IOPORT_IRQ_*, AVR_IOCTL_IOPORT_* and state structs

/* Passed as 'vport' to avr_port_modern_init() for a port with no VPORT alias
 * (VPORTA legitimately lives at data address 0x0000, so 0 is not the sentinel). */
#define AVR_PORT_MODERN_NO_VPORT 0xffff

/* Register offsets within a PORT block (matches the device header PORT_t). */
enum {
	PORTM_DIR = 0x00,
	PORTM_DIRSET = 0x01,
	PORTM_DIRCLR = 0x02,
	PORTM_DIRTGL = 0x03,
	PORTM_OUT = 0x04,
	PORTM_OUTSET = 0x05,
	PORTM_OUTCLR = 0x06,
	PORTM_OUTTGL = 0x07,
	PORTM_IN = 0x08,
	PORTM_INTFLAGS = 0x09,
	/* Multi-pin configuration, AVR Dx only (DS40002413B 18.5.12-15). */
	PORTM_PINCONFIG = 0x0b,
	PORTM_PINCTRLUPD = 0x0c,
	PORTM_PINCTRLSET = 0x0d,
	PORTM_PINCTRLCLR = 0x0e,
	PORTM_PIN0CTRL = 0x10,	/* PIN0CTRL .. PIN7CTRL at 0x10 .. 0x17 */
};

/* PINnCTRL bits */
#define PINCTRL_ISC	0x07
#define PINCTRL_PULLUPEN	0x08
#define PINCTRL_INVEN	0x80

/* Offsets within a VPORT block (device header VPORT_t). */
enum {
	VPORTM_DIR = 0x00,
	VPORTM_OUT = 0x01,
	VPORTM_IN = 0x02,
	VPORTM_INTFLAGS = 0x03,
};

typedef struct avr_port_modern_t {
	avr_io_t	io;
	char		name;		// 'A', 'B', 'C', …

	avr_io_addr_t	base;	// PORT base (e.g. 0x400)
	avr_io_addr_t	vport;	// VPORT base in low I/O (e.g. 0x00); 0 = none

	/* Resolved canonical register addresses (base + offset). */
	avr_io_addr_t	r_dir, r_out, r_in, r_intflags, r_pinctrl;

	avr_int_vector_t	port_vect;	// PORTx_PORT interrupt (pin interrupts)

	uint8_t		irqing;		// suppress PORT-register IRQ recursion

	/* Multi-pin configuration (AVR Dx): PINCONFIG is mirrored across all the
	 * ports in mp_ports (see avr_port_modern_enable_multipin()). */
	struct avr_port_modern_t	**mp_ports;
	int			mp_count;

	/* Peripheral pin overrides (see avr_port_modern_override()). Pins in
	 * ovr_en take their output level from ovr_val instead of OUT; pins in
	 * ovr_dir are additionally forced to output. DIR/OUT themselves are never
	 * modified, so releasing a pin restores what firmware configured. */
	uint8_t		ovr_en, ovr_val, ovr_dir;

	/* Default level for input pins driven from outside (see IOPORT external). */
	struct {
		uint8_t pull_mask, pull_value;
	} external;
} avr_port_modern_t;

/*
 * Initialise a modern PORT at data address 'base', with its VPORT alias at
 * 'vport' (pass 0 for no VPORT). 'vector' is the PORTx_PORT interrupt vector
 * number; 'name' is the uppercase port letter used for the IOPORT ioctls.
 */
void
avr_port_modern_init(
		avr_t * avr,
		avr_port_modern_t * p,
		char name,
		avr_io_addr_t base,
		avr_io_addr_t vport,
		uint8_t vector);

/*
 * A peripheral takes over (en != 0) or releases (en == 0) pin 'pin' (0-7).
 * 'value' is the level it drives; 'force_output' also forces the pin to be an
 * output regardless of DIR (otherwise DIR still decides, as for TCA waveform
 * outputs, where the user must set DIR). Re-derives the pin IRQs.
 */
void
avr_port_modern_override(
		avr_port_modern_t * p,
		int pin,
		int en,
		int force_output,
		int value);

/*
 * Enable the AVR Dx multi-pin configuration registers (PINCONFIG, PINCTRLUPD,
 * PINCTRLSET, PINCTRLCLR) on 'count' ports. PINCONFIG is mirrored across them.
 * Call after avr_port_modern_init() of every port; 'ports' must outlive them.
 */
void
avr_port_modern_enable_multipin(
		avr_t * avr,
		avr_port_modern_t ** ports,
		int count);

#ifdef __cplusplus
};
#endif

#endif /* __AVR_PORT_MODERN_H__ */
