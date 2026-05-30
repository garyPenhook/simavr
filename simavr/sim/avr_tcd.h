/*
	avr_tcd.h

	"Modern" AVR (AVRxt) 12-bit Timer/Counter type D (TCD0 at 0x0A80 on the
	tinyAVR 1-series, also AVR Dx). TCD is an asynchronous timer aimed at
	high-resolution PWM; this models its periodic-overflow use.

	The counter runs from a prescaled clock (CLKSEL -> SYNCPRES -> CNTPRES) and,
	in one-ramp operation, completes a cycle every (CMPBCLR + 1) counts, at which
	point the OVF flag is set and TCD0_OVF raised (if enabled). The double-
	buffered enable/command protocol is satisfied by reporting STATUS.ENRDY and
	CMDRDY always ready, so the usual `while (!(TCD0.STATUS & ENRDY))` polling
	passes.

	Not modelled: the waveform outputs (WOA/WOB), TRIGA/TRIGB compare events,
	dithering, fault control, input capture, and the exact TCD clock source
	(approximated as CLK_PER); those registers still store.

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

#ifndef __AVR_TCD_H__
#define __AVR_TCD_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "sim_avr.h"

/* Register offsets within a TCD block (device header TCD_t). */
enum {
	TCDR_CTRLA = 0x00,
	TCDR_CTRLB = 0x01,
	TCDR_CTRLE = 0x04,
	TCDR_INTCTRL = 0x0c,
	TCDR_INTFLAGS = 0x0d,
	TCDR_STATUS = 0x0e,
	TCDR_CMPBCLRL = 0x2e,	/* 16-bit (12-bit value) TOP */
	TCDR_CMPBCLRH = 0x2f,
};

typedef struct avr_tcd_t {
	avr_io_t	io;
	char		name;

	avr_io_addr_t	base;
	avr_io_addr_t	r_ctrla, r_intctrl, r_intflags, r_status, r_cmpbclr;

	avr_int_vector_t	ovf;	/* TCDn_OVF */

	avr_cycle_count_t	start_cycle;
	uint32_t		prescale;	/* CPU cycles per TCD count */
	uint32_t		top;		/* CMPBCLR captured at start */
} avr_tcd_t;

/*
 * Initialise a TCD block at data address 'base'. 'vec_ovf' is the TCDn_OVF
 * vector; 'name' is a tag for debug.
 */
void
avr_tcd_init(
		avr_t * avr,
		avr_tcd_t * p,
		avr_io_addr_t base,
		uint8_t vec_ovf,
		char name);

#ifdef __cplusplus
};
#endif

#endif /* __AVR_TCD_H__ */
