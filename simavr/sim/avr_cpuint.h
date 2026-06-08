/*
	avr_cpuint.h

	"Modern" AVR (AVRxt) CPU Interrupt Controller (CPUINT at 0x0110 on the
	tinyAVR 1-series and megaAVR-0 families; also AVR Dx).

	The interrupt *engine* (priority levels, NMI, preemption, round robin,
	modified-static priority via LVL0PRI, RETI level-flag handling) lives in
	sim_interrupts.c and is parameterised by the fields in avr_int_table_t.
	This module is the firmware-facing register block that drives those fields:
	it maps CPUINT.CTRLA / STATUS / LVL0PRI / LVL1VEC (data offsets 0x00..0x03)
	onto the engine so firmware can actually configure interrupt priority.

	Register semantics (ATtiny3216/17 datasheet DS40002205A §13.4-13.5):
	  CTRLA  : IVSEL (bit6, CCP), CVT (bit5, CCP), LVL0RR (bit0, not CCP)
	  STATUS : NMIEX (bit7), LVL1EX (bit1), LVL0EX (bit0) — all read-only,
	           set/cleared by hardware, mirrored here from the engine
	  LVL0PRI: LVL0 scheduling base / last-acked vector (hardware-updated under
	           round robin), read-back returns the live value
	  LVL1VEC: vector number elevated to priority level 1 (0 = none)

	IVSEL relocates the vector table (DS40002205A 13.5.1): IVSEL=1 places it at
	the start of the boot section (flash 0x0000), IVSEL=0 at the start of the
	application section (flash FUSE.BOOTEND*256). The engine applies this base in
	avr_service_interrupts_modern(); with the default BOOTEND=0 the whole flash is
	boot, the app section starts at 0, and IVSEL has no effect. CVT is also fully
	modelled in the engine.

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

#ifndef __AVR_CPUINT_H__
#define __AVR_CPUINT_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "sim_avr.h"

/* Register offsets within a CPUINT block (device header CPUINT_t). */
enum {
	CPUINTR_CTRLA	= 0x00,
	CPUINTR_STATUS	= 0x01,
	CPUINTR_LVL0PRI	= 0x02,
	CPUINTR_LVL1VEC	= 0x03,
};

typedef struct avr_cpuint_t {
	avr_io_t	io;
	char		name;

	avr_io_addr_t	base;
	avr_io_addr_t	r_ctrla;
	avr_io_addr_t	r_status;
	avr_io_addr_t	r_lvl0pri;
	avr_io_addr_t	r_lvl1vec;
} avr_cpuint_t;

/*
 * Initialise a CPUINT block at data address 'base'. 'name' is a tag for debug.
 * 'bootend_fuse_index' locates FUSE.BOOTEND so IVSEL can relocate the vector
 * table to the boot/application section (0xff to disable relocation).
 * Requires the core to have AVR_ARCH_F_CPUINT set (modern dispatch).
 */
void
avr_cpuint_init(
		avr_t * avr,
		avr_cpuint_t * p,
		avr_io_addr_t base,
		uint8_t bootend_fuse_index,
		char name);

#ifdef __cplusplus
};
#endif

#endif /* __AVR_CPUINT_H__ */
