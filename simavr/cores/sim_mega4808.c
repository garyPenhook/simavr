/*
	sim_mega4808.c

	ATmega4808 (megaAVR(R) 0-series, AVRxt). Thin instantiation of the shared
	megaAVR-0 core template; see sim_megax08.h for the device model.

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

#define SIM_MMCU		"atmega4808"
#define SIM_CORENAME	mcu_mega4808

#define _AVR_IO_H_
#define __ASSEMBLER__
#define _SFR_MEM8(x)	(x)
#define _SFR_MEM16(x)	(x)
#define _SFR_IO8(x)		(x)
#define _SFR_IO16(x)	(x)
#include "avr/iom4808.h"

#include "sim_megax08.h"

avr_kind_t mega4808 = {
	.names = { "atmega4808" },
	.make = sim_megax08_make,
};
