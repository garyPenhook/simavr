/*
	sim_tiny3217.c

	ATtiny3217 (tinyAVR 1-series) core for simavr. See sim_tiny3217.h.

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

#include "sim_avr.h"

#define SIM_VECTOR_SIZE	4		// 32 KB flash (>8 KW) uses JMP vectors
#define SIM_MMCU		"attiny3217"
#define SIM_CORENAME	mcu_tiny3217

#include "sim_tiny3217.h"

void t3217_init(struct avr_t * avr)
{
	struct mcu_t * mcu = (struct mcu_t *)avr;

	avr_port_init(avr, &mcu->porta);
	avr_port_init(avr, &mcu->portb);
	avr_port_init(avr, &mcu->portc);
}

void t3217_reset(struct avr_t * avr)
{
}

static avr_t * make()
{
	return avr_core_allocate(&SIM_CORENAME.core, sizeof(struct mcu_t));
}

avr_kind_t tiny3217 = {
	.names = { "attiny3217" },
	.make = make
};
