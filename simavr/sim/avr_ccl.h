/*
	avr_ccl.h

	"Modern" AVR (AVRxt) Configurable Custom Logic (CCL at 0x01C0 on the tinyAVR
	1-series, also megaAVR-0 and AVR Dx families).

	Models the combinational core of the look-up tables: each LUT computes a
	3-input truth table (TRUTHn) whose inputs are routed by LUTnCTRLB/C.INSEL.
	The routed sources that are modelled are MASK (constant 0), IO (an external
	level presented on the LUTn-INm IRQ), LINK (the next LUT's output) and
	FEEDBACK (the LUT's own output); other sources (events, peripherals) read as
	0. Each LUT output is published on its OUT IRQ when it changes; combinational
	LINK/FEEDBACK loops are settled to a fixed point.

	Not modelled: the synchronizer/filter (LUTnCTRLA.FILTSEL), edge detector
	(EDGEDET), clock source (CLKSRC) and the sequencer (SEQCTRL0) — those
	registers still store, so configuring firmware behaves; only the timing/
	stateful behaviour is absent.

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

#ifndef __AVR_CCL_H__
#define __AVR_CCL_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "sim_avr.h"

#define AVR_CCL_MAX_LUTS	4
#define AVR_CCL_LUT_INPUTS	3

/* Register offsets within a CCL block (device header CCL_t). */
enum {
	CCLR_CTRLA = 0x00,
	CCLR_SEQCTRL0 = 0x01,
	CCLR_LUT0CTRLA = 0x05,	/* per-LUT block stride is 4 bytes */
};

/*
 * IRQs. Inputs (drive an external level 0/1): LUTn-INm at index n*3 + m.
 * Outputs (observed): LUTn OUT at index nluts*3 + n. For the 2-LUT ATtiny3217
 * the named indices below apply.
 */
enum {
	AVR_CCL_IRQ_LUT0_IN0 = 0,
	AVR_CCL_IRQ_LUT0_IN1,
	AVR_CCL_IRQ_LUT0_IN2,
	AVR_CCL_IRQ_LUT1_IN0,
	AVR_CCL_IRQ_LUT1_IN1,
	AVR_CCL_IRQ_LUT1_IN2,
	AVR_CCL_IRQ_LUT0_OUT,
	AVR_CCL_IRQ_LUT1_OUT,
	AVR_CCL_IRQ_COUNT_2LUT,
};

typedef struct avr_ccl_t {
	avr_io_t	io;
	char		name;		/* '0', … */

	avr_io_addr_t	base;
	avr_io_addr_t	r_ctrla;
	uint8_t		nluts;

	uint8_t		out[AVR_CCL_MAX_LUTS];	/* current logic output of each LUT */
	uint8_t		pub[AVR_CCL_MAX_LUTS];	/* last value published on OUT IRQ */
	uint8_t		io_in[AVR_CCL_MAX_LUTS][AVR_CCL_LUT_INPUTS];	/* IO-source levels */
	int			base_irq;
} avr_ccl_t;

/*
 * Initialise a CCL block at data address 'base' with 'nluts' look-up tables
 * (2 on the ATtiny3217). 'name' is a tag for debug and the IRQ ioctl.
 */
void
avr_ccl_init(
		avr_t * avr,
		avr_ccl_t * p,
		avr_io_addr_t base,
		uint8_t nluts,
		char name);

#define AVR_IOCTL_CCL_GETIRQ(_name) AVR_IOCTL_DEF('c','c','l',(_name))

#ifdef __cplusplus
};
#endif

#endif /* __AVR_CCL_H__ */
