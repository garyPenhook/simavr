/*
	sim_tiny3216.c

	ATtiny3216 (tinyAVR(R) 1-series, AVRxt). Thin instantiation of the shared
	1-series core template; see sim_tinyx1.h for the device model.

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

#define SIM_MMCU		"attiny3216"
#define SIM_CORENAME	mcu_tiny3216

#define _AVR_IO_H_
#define __ASSEMBLER__
#define _SFR_MEM8(x)	(x)
#define _SFR_MEM16(x)	(x)
#define _SFR_IO8(x)		(x)
#define _SFR_IO16(x)	(x)
#include "avr/iotn3216.h"

/* DS40002205A Table 5-1 / PORTMUX.CTRLC: TCA0 WOn default pin, alternate pin. */
#define SIM_PORTMUX_TCA0_WO { { { 'B', 0 }, { 'B', 3 } }, \
                              { { 'B', 1 }, { 'B', 4 } }, \
                              { { 'B', 2 }, { 'B', 5 } } }

/* DS40002205A Table 5-1 / PORTMUX.CTRLB: USART0 TxD default (PB2), alternate (PA1). */
#define SIM_PORTMUX_USART0_TXD { { 'B', 2 }, { 'A', 1 } }

#include "sim_tinyx1.h"

avr_kind_t tiny3216 = {
	.names = { "attiny3216" },
	.make = sim_tinyx1_make,
};
