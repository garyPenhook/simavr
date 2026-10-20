/*
	avr_clkpr.h

	Classic AVR system clock prescaler (CLKPR), as found on the ATtiny13A and
	most other tinyAVR/megaAVR parts that have CLKPR. CLKPS[3:0] divides the
	selected clock source by 2^n (n = 0..8); the protected write sequence
	(CLKPCE, then CLKPS within four cycles) and the CKDIV8 fuse reset value
	follow ATtiny13A DS40002307A section 6.4.2 and Table 6-8.

	avr->frequency is the CPU clock the firmware runs at after reset (what an
	.mmcu section or --freq gives), so the model keeps that meaning: a CLKPR
	write rescales avr->frequency by old/new division, and a reset puts it back.
	Cycle-time conversions and timers registered afterwards see the new clock.
	The frequency you give is therefore the divided one: with the default
	CKDIV8 fuse a 9.6 MHz RC oscillator starts at 1.2 MHz, so pass 1200000
	(AVR_MCU(1200000, ...)); passing 9600000 would make the start-up clock 8x too fast.

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

#ifndef __AVR_CLKPR_H__
#define __AVR_CLKPR_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "sim_avr.h"

typedef struct avr_clkpr_t {
	avr_io_t	io;
	avr_io_addr_t	r_clkpr;
	uint8_t		ckdiv8_fuse_byte;	// which fuse byte holds CKDIV8
	uint8_t		ckdiv8_fuse_bit;	// its bit; 0 = programmed = divide by 8 at reset

	uint16_t	div;			// division in force (1..256)
	uint16_t	reset_div;		// division the last reset started with
	uint64_t	base_hz;		// undivided source clock: frequency * div, kept exact
	uint32_t	last_hz;		// avr->frequency as this module last set it
} avr_clkpr_t;

void avr_clkpr_init(avr_t * avr, avr_clkpr_t * p, avr_io_addr_t r_clkpr,
		uint8_t ckdiv8_fuse_byte, uint8_t ckdiv8_fuse_bit);

#ifdef __cplusplus
};
#endif

#endif /*__AVR_CLKPR_H__*/
