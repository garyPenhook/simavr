/*
	avr_slpctrl.h

	"Modern" AVR (AVRxt) Sleep Controller (SLPCTRL at 0x0050 on the tinyAVR
	1-series, also megaAVR-0 and AVR Dx families).

	SLPCTRL.CTRLA holds the sleep enable (SEN) and the sleep mode (SMODE: IDLE,
	STANDBY, POWER-DOWN). On modern cores a SLEEP instruction only puts the CPU
	to sleep while SEN is set; this module maintains the engine's
	avr->arch.sleep_enabled flag from SEN so the modern SLEEP path honours it.
	(Wake-up is the engine's existing behaviour: a serviceable interrupt resumes
	execution.) SMODE is stored; which peripherals keep running per mode is not
	modelled.

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

#ifndef __AVR_SLPCTRL_H__
#define __AVR_SLPCTRL_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "sim_avr.h"

/* Register offsets within a SLPCTRL block (device header SLPCTRL_t). */
enum {
	SLPCTRLR_CTRLA = 0x00,
};

typedef struct avr_slpctrl_t {
	avr_io_t	io;
	char		name;

	avr_io_addr_t	base;
	avr_io_addr_t	r_ctrla;
} avr_slpctrl_t;

/*
 * Initialise a SLPCTRL block at data address 'base'. 'name' is a tag for debug.
 */
void
avr_slpctrl_init(
		avr_t * avr,
		avr_slpctrl_t * p,
		avr_io_addr_t base,
		char name);

#ifdef __cplusplus
};
#endif

#endif /* __AVR_SLPCTRL_H__ */
