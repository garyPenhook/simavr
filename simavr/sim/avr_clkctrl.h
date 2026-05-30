/*
	avr_clkctrl.h

	"Modern" AVR (AVRxt) Clock Controller (CLKCTRL), as found on the tinyAVR
	1-series (ATtiny3217), megaAVR-0 and AVR Dx families.

	This models the main-clock selection (MCLKCTRLA.CLKSEL) and prescaler
	(MCLKCTRLB.PEN/PDIV) and keeps avr->frequency in sync, so that cycle-time
	conversions and timers see the clock the firmware actually configured. The
	clock registers are Configuration-Change-Protected; writes are honoured only
	inside a CCP unlock window (and once MCLKLOCK.LOCKEN is set they become
	read-only until reset).

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

#ifndef __AVR_CLKCTRL_H__
#define __AVR_CLKCTRL_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "sim_avr.h"

/* Register offsets within the CLKCTRL block (device header CLKCTRL_t). */
enum {
	CLKCTRL_MCLKCTRLA_O = 0x00,
	CLKCTRL_MCLKCTRLB_O = 0x01,
	CLKCTRL_MCLKLOCK_O = 0x02,
	CLKCTRL_MCLKSTATUS_O = 0x03,
};

typedef struct avr_clkctrl_t {
	avr_io_t	io;

	avr_io_addr_t	base;	// CLKCTRL base (e.g. 0x60)
	avr_io_addr_t	r_mclkctrla, r_mclkctrlb, r_mclklock, r_mclkstatus;

	/* Source oscillator frequencies (Hz). */
	uint32_t	freq_osc20m;	// 16M or 20M; if 0, derived from FUSE.OSCCFG
	uint32_t	freq_osc32k;	// internal/external 32.768 kHz
	uint32_t	freq_extclk;	// external clock pin; 0 = leave unchanged
} avr_clkctrl_t;

/*
 * Initialise the CLKCTRL block at data address 'base'.
 * 'osccfg_fuse_index' is the index into avr->fuse[] of the OSCCFG fuse (its
 * FREQSEL field selects the 16/20 MHz internal oscillator). Pass 0xff to force
 * the 20 MHz default.
 */
void
avr_clkctrl_init(
		avr_t * avr,
		avr_clkctrl_t * p,
		avr_io_addr_t base,
		uint8_t osccfg_fuse_index);

#ifdef __cplusplus
};
#endif

#endif /* __AVR_CLKCTRL_H__ */
