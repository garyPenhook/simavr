/*
	sim_tiny3217.h

	ATtiny3217 (tinyAVR 1-series, AVRxt core) descriptor for simavr.

	This is the first modern-AVR core. Unlike the classic cores it does not lean
	on the avr-libc io header for register addresses (the modern header is
	struct-based); the few addresses needed are taken directly from the
	datasheet (DS40002205A) and spelled out here. Engine support for the modern
	memory/IO model, AVRxt timing, CCP and the CPUINT interrupt controller lives
	behind the avr->arch.flags set below.

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

#ifndef __SIM_TINY3217_H__
#define __SIM_TINY3217_H__

#include "sim_avr.h"
#include "avr_port.h"

// --- ATtiny3217 memory map (data space) ---
#define T3217_FLASHEND	0x7FFF		// 32 KB
#define T3217_RAMSTART	0x3800		// 2 KB SRAM at 0x3800..0x3FFF
#define T3217_RAMEND	0x3FFF
#define T3217_E2SIZE	256			// EEPROM bytes
#define T3217_FLASHMAP	0x8000		// flash mapped into data space here

// CPU registers (modern layout)
#define T3217_CCP	0x0034
#define T3217_SPL	0x003D
#define T3217_SREG	0x003F

// RSTCTRL.RSTFR (reset flags)
#define T3217_RSTFR	0x0040

// PORT base addresses
#define T3217_PORTA	0x0400
#define T3217_PORTB	0x0420
#define T3217_PORTC	0x0440

void t3217_init(struct avr_t * avr);
void t3217_reset(struct avr_t * avr);

struct mcu_t {
	avr_t		core;
	avr_port_t	porta, portb, portc;
};

#ifdef SIM_CORENAME

#ifndef SIM_VECTOR_SIZE
#error SIM_VECTOR_SIZE is not declared
#endif
#ifndef SIM_MMCU
#error SIM_MMCU is not declared
#endif

const struct mcu_t SIM_CORENAME = {
	.core = {
		.mmcu = SIM_MMCU,
		.ioend = T3217_RAMSTART - 1,
		.ramend = T3217_RAMEND,
		.flashend = T3217_FLASHEND,
		.e2end = T3217_E2SIZE - 1,
		.vector_size = SIM_VECTOR_SIZE,
		.signature = { 0x1E, 0x95, 0x22 },
		.lockbits = 0xFF,

		// Modern (AVRxt) architecture parameters. avr_init() leaves these as-is
		// because AVR_ARCH_F_MODERN is set.
		.arch = {
			.flags = AVR_ARCH_F_MODERN | AVR_ARCH_F_CCP |
					 AVR_ARCH_F_XT_TIMING | AVR_ARCH_F_CPUINT,
			.io_offset = 0,
			.sp_addr = T3217_SPL,
			.sreg_addr = T3217_SREG,
			.ccp_addr = T3217_CCP,
			.flashmap_start = T3217_FLASHMAP,
		},

		// Reset flags live in RSTCTRL.RSTFR, not the classic MCUSR.
		.reset_flags = {
			.porf  = AVR_IO_REGBIT(T3217_RSTFR, 0),
			.extrf = AVR_IO_REGBIT(T3217_RSTFR, 1),
			.borf  = AVR_IO_REGBIT(T3217_RSTFR, 2),
			.wdrf  = AVR_IO_REGBIT(T3217_RSTFR, 3),
		},

		.init = t3217_init,
		.reset = t3217_reset,
	},
	.porta = { .name = 'A', .r_base = T3217_PORTA },
	.portb = { .name = 'B', .r_base = T3217_PORTB },
	.portc = { .name = 'C', .r_base = T3217_PORTC },
};

#endif /* SIM_CORENAME */

#endif /* __SIM_TINY3217_H__ */
