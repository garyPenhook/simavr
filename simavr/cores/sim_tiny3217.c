/*
	sim_tiny3217.c

	ATtiny3217 (tinyAVR(R) 1-series, AVRxt). Thin instantiation of the shared
	1-series core template; see sim_tinyx1.h for the device model and doc/attiny3217_design.md.

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

#include "sim_avr.h"

#define SIM_MMCU		"attiny3217"
#define SIM_CORENAME	mcu_tiny3217

#define _AVR_IO_H_
#define __ASSEMBLER__
#define _SFR_MEM8(x)	(x)
#define _SFR_MEM16(x)	(x)
#define _SFR_IO8(x)		(x)
#define _SFR_IO16(x)	(x)
#include "avr/iotn3217.h"

/* DS40002205A Table 5-1 / PORTMUX.CTRLC: TCA0 WOn default pin, alternate pin. */
#define SIM_PORTMUX_TCA0_WO { { { 'B', 0 }, { 'B', 3 } }, \
                              { { 'B', 1 }, { 'B', 4 } }, \
                              { { 'B', 2 }, { 'B', 5 } } }

/* Split mode WO3-5 (PORTMUX.CTRLC TCA03-TCA05): PA3-5 by default, PC3-5 alternate. */
#define SIM_PORTMUX_TCA0_WO345 { { { 'A', 3 }, { 'C', 3 } }, \
                                 { { 'A', 4 }, { 'C', 4 } }, \
                                 { { 'A', 5 }, { 'C', 5 } } }

/* DS40002205A Table 5-1 / PORTMUX.CTRLB: USART0 TxD default (PB2), alternate (PA1). */
#define SIM_PORTMUX_USART0_TXD { { 'B', 2 }, { 'A', 1 } }

#include "sim_tinyx1.h"

avr_kind_t tiny3217 = {
	.names = { "attiny3217" },
	.make = sim_tinyx1_make,
};
