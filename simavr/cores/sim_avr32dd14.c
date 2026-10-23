/*
	sim_avr32dd14.c

	AVR32DD14 (AVR DD family, AVRxt). Thin instantiation of the shared AVR DD core
	template; see sim_avrdd.h for the device model.

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

#define SIM_MMCU		"avr32dd14"
#define SIM_CORENAME	mcu_avr32dd14

#define _AVR_IO_H_
#define __ASSEMBLER__
#define _SFR_MEM8(x)	(x)
#define _SFR_MEM16(x)	(x)
#define _SFR_IO8(x)		(x)
#define _SFR_IO16(x)	(x)
#include "avr/ioavr32dd14.h"

#include "sim_avrdd.h"

avr_kind_t avr32dd14 = {
	.names = { "avr32dd14" },
	.make = sim_avrdd_make,
};
