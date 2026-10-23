/*
	avr_clkctrl_dd.h

	AVR DD (and the other AVR Dx) Clock Controller. Unlike the tinyAVR/megaAVR
	CLKCTRL (avr_clkctrl.h) it has a programmable 1-24 MHz OSCHF, a 32.768 kHz
	internal and external crystal oscillator, an external HF clock/crystal, a PLL
	and clock failure detection. Register layout and behaviour follow
	DS40002413B section 12 (AVR16/32DD14/20).

	Modelled: main clock source selection (MCLKCTRLA.CLKSEL), the prescaler
	(MCLKCTRLB), OSCHF frequency select, MCLKSTATUS, W1C MCLKINTFLAGS, the CCP
	protection of every protected register. External sources only take effect once
	they are enabled, and until then MCLKSTATUS.SOSC stays set (DS p.91). Not
	modelled: the PLL output (it feeds peripherals such as TCD, not CLK_MAIN), auto
	tune, and clock failure detection.

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

#ifndef __AVR_CLKCTRL_DD_H__
#define __AVR_CLKCTRL_DD_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "sim_avr.h"

/* Register offsets within the CLKCTRL block (DS40002413B 12.4). */
enum {
	CLKCTRL_DD_MCLKCTRLA_O		= 0x00,
	CLKCTRL_DD_MCLKCTRLB_O		= 0x01,
	CLKCTRL_DD_MCLKCTRLC_O		= 0x02,
	CLKCTRL_DD_MCLKINTCTRL_O	= 0x03,
	CLKCTRL_DD_MCLKINTFLAGS_O	= 0x04,
	CLKCTRL_DD_MCLKSTATUS_O		= 0x05,
	CLKCTRL_DD_OSCHFCTRLA_O		= 0x08,
	CLKCTRL_DD_OSCHFTUNE_O		= 0x09,
	CLKCTRL_DD_PLLCTRLA_O		= 0x10,
	CLKCTRL_DD_OSC32KCTRLA_O	= 0x18,
	CLKCTRL_DD_XOSC32KCTRLA_O	= 0x1c,
	CLKCTRL_DD_XOSCHFCTRLA_O	= 0x20,
};

typedef struct avr_clkctrl_dd_t {
	avr_io_t	io;

	avr_io_addr_t	base;
	/* Frequency of the external clock/crystal on XTALHF1/2 (Hz); 0 = unknown, in
	 * which case selecting EXTCLK leaves avr->frequency unchanged. */
	uint32_t	freq_extclk;
	/* Frequency of the XOSC32K crystal/clock (Hz). */
	uint32_t	freq_xosc32k;
	uint8_t		osccfg_fuse;	// index of FUSE.OSCCFG in avr->fuse[]
} avr_clkctrl_dd_t;

/* 'osccfg_fuse_index' is the index of FUSE.OSCCFG (0xff = OSCHF default). */
void
avr_clkctrl_dd_init(
		avr_t * avr,
		avr_clkctrl_dd_t * p,
		avr_io_addr_t base,
		uint8_t osccfg_fuse_index);

#ifdef __cplusplus
};
#endif

#endif /* __AVR_CLKCTRL_DD_H__ */
