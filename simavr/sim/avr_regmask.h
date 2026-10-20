/*
	avr_regmask.h

	Reserved bits and special write rules for the plain registers of a core.

	A register's reserved bits "always read as zero"; MCUSR flags can only be
	cleared by software; BODCR is written through a timed sequence. This block
	applies such rules on top of whatever peripheral model owns the register.

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

#ifndef __AVR_REGMASK_H__
#define __AVR_REGMASK_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "sim_avr.h"

#define AVR_REGMASK_MAX		24

enum {
	AVR_REGMASK_PLAIN = 0,
	AVR_REGMASK_CLEAR_ONLY,		// a write of 1 can not set a bit (reset flags in MCUSR)
	AVR_REGMASK_BODCR,		// BODS/BODSE timed sequence (bit 1 / bit 0)
};

typedef struct avr_regmask_entry_t {
	avr_io_addr_t	reg;
	uint8_t		mask;		// implemented bits; the others read as zero
	uint8_t		kind;
	uint8_t		shared;		// another model already handles writes to this register
	struct avr_regmask_t *	owner;
} avr_regmask_entry_t;

typedef struct avr_regmask_t {
	avr_io_t		io;
	avr_regmask_entry_t	entry[AVR_REGMASK_MAX];	// terminated by reg == 0
} avr_regmask_t;

/* Register the entries; call it after every peripheral that owns one of the registers. */
void avr_regmask_init(avr_t * avr, avr_regmask_t * port);

#ifdef __cplusplus
};
#endif

#endif /* __AVR_REGMASK_H__ */
