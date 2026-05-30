/*
	avr_rstctrl.h

	"Modern" AVR (AVRxt) Reset Controller (RSTCTRL at 0x0040 on the tinyAVR
	1-series, also megaAVR-0 and AVR Dx families).

	RSTCTRL.RSTFR records the cause of the last reset (PORF, BORF, EXTRF, WDRF,
	SWRF, UPDIRF — write-1-to-clear), and RSTCTRL.SWRR triggers a software reset
	when SWRE is written. This models:
	  * the power-on flag (PORF) set at the initial power-up,
	  * a software reset (SWRR.SWRE) that resets the device and sets RSTFR.SWRF,
	  * RSTFR write-1-to-clear.

	Not modelled as causes: BOR/external/UPDI resets, and a WDT timeout does not
	set WDRF (the WDT models the reset effect, not the cause flag).

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

#ifndef __AVR_RSTCTRL_H__
#define __AVR_RSTCTRL_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "sim_avr.h"

/* Register offsets within an RSTCTRL block (device header RSTCTRL_t). */
enum {
	RSTCTRLR_RSTFR = 0x00,
	RSTCTRLR_SWRR = 0x01,
};

typedef struct avr_rstctrl_t {
	avr_io_t	io;
	char		name;

	avr_io_addr_t	base;
	avr_io_addr_t	r_rstfr, r_swrr;

	uint8_t		pending_cause;	/* RSTFR bits to set on the next reset */
	uint8_t		powered_on;	/* PORF already applied (first reset done) */
	uint8_t		sw_reset_pending;
	avr_run_t	saved_run;	/* run callback saved during a software reset */
} avr_rstctrl_t;

/*
 * Initialise an RSTCTRL block at data address 'base'. 'name' is a tag for debug.
 */
void
avr_rstctrl_init(
		avr_t * avr,
		avr_rstctrl_t * p,
		avr_io_addr_t base,
		char name);

#ifdef __cplusplus
};
#endif

#endif /* __AVR_RSTCTRL_H__ */
