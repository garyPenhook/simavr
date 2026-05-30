/*
	sim_core_declare_modern.h

	Core-descriptor helper for "modern" AVR (AVRxt) parts: tinyAVR 1-series,
	megaAVR-0, AVR Dx. These differ from classic AVRs in the data/IO memory
	model, instruction timing, Configuration Change Protection and the CPUINT
	interrupt controller (see doc/attiny3217_design.md). The classic
	DEFAULT_CORE() macro bakes in classic-AVR assumptions, so modern cores use
	MODERN_CORE() instead.

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
#ifndef __SIM_CORE_DECLARE_MODERN_H__
#define __SIM_CORE_DECLARE_MODERN_H__

/* Reuse the _SFR/_VECTOR/_BV fakes (and the FUSE plumbing we don't use). */
#include "sim_core_declare.h"

/*
 * Modern parts place all peripheral registers in the extended I/O space
 * (0x0040..0x1FFF on the ATtiny3217); SRAM starts at RAMSTART (0x3800). The
 * io[] callback table must cover the whole register span, so ioend is the top
 * of extended I/O rather than RAMSTART-1.
 */
#ifndef MODERN_IOEND
#define MODERN_IOEND 0x1FFF
#endif

/*
 * Standard modern-AVR low-I/O fixed addresses (data space):
 *   CCP  = 0x34, SP = 0x3D/0x3E, SREG = 0x3F.
 */
#define MODERN_CCP_ADDR		0x34
#define MODERN_SP_ADDR		0x3D
#define MODERN_SREG_ADDR	0x3F

/*
 * _vector_size: 4 for parts whose toolchain emits JMP-based vectors (>=8KW
 * flash, e.g. ATtiny3217), 2 otherwise. Confirm with avr-objdump of the linked
 * vector table.
 */
#define MODERN_CORE(_vector_size) \
	.ioend  = MODERN_IOEND, \
	.frequency = 3333333, /* OSC20M / 6, the reset default; CLKCTRL updates it */ \
	.ramend = RAMEND, \
	.flashend = FLASHEND, \
	.e2end = E2END, \
	.vector_size = _vector_size, \
	.signature = { SIGNATURE_0, SIGNATURE_1, SIGNATURE_2 }, \
	.lockbits = 0xFF, \
	.arch = { \
		.flags = AVR_ARCH_F_MODERN | AVR_ARCH_F_CCP | \
				 AVR_ARCH_F_XT_TIMING | AVR_ARCH_F_CPUINT, \
		.io_offset = 0, \
		.sp_addr = MODERN_SP_ADDR, \
		.sreg_addr = MODERN_SREG_ADDR, \
		.ccp_addr = MODERN_CCP_ADDR, \
		.flashmap_start = MAPPED_PROGMEM_START, \
	}

#endif /* __SIM_CORE_DECLARE_MODERN_H__ */
