/*
	sim_tiny212.c

	ATtiny212 (tinyAVR(R) 1-series, AVRxt). Thin instantiation of the shared
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

#define SIM_MMCU		"attiny212"
#define SIM_CORENAME	mcu_tiny212

#define _AVR_IO_H_
#define __ASSEMBLER__
#define _SFR_MEM8(x)	(x)
#define _SFR_MEM16(x)	(x)
#define _SFR_IO8(x)		(x)
#define _SFR_IO16(x)	(x)
#include "avr/iotn212.h"

#include "sim_tinyx1.h"

avr_kind_t tiny212 = {
	.names = { "attiny212" },
	.make = sim_tinyx1_make,
};
