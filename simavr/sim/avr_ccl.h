/*
	avr_ccl.h

	"Modern" AVR (AVRxt) Configurable Custom Logic (CCL at 0x01C0 on the tinyAVR
	1-series, also megaAVR-0 and AVR Dx families).

	Models the LUT truth table plus the stateful post-processing stages:
	synchronizer/filter (FILTSEL), rising-edge detector (EDGEDET), alternate
	clocking from IN2 (CLKSRC=IN2), and the per-pair sequencer (SEQCTRLn: DFF,
	JK, gated latch, RS latch). LINK uses the next LUT's direct output; FEEDBACK
	uses the pair sequencer output. The INSEL event/peripheral sources (events,
	AC/TCB/TCA/TCD/USART/SPI outputs) are decoded per family and resolved from a
	cached level driven on the block's source IRQs (see avr_ccl_src); sources not
	fitted on a device are never driven and read 0. Each LUT output is published
	on its OUT IRQ when it changes, and combinational LINK/FEEDBACK loops are
	settled to a fixed point before the clocked stages run.

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
 * the named indices below apply. After the per-LUT IO IRQs come the shared
 * event/peripheral source IRQs (see avr_ccl_src below), used to drive the
 * INSEL event/peripheral sources; their base is AVR_CCL_LUT_IRQS(nluts).
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
	AVR_CCL_IRQ_COUNT_4LUT = 16,
};

/* IO input + output IRQs occupy this many slots for an nluts-LUT block. */
#define AVR_CCL_LUT_IRQS(nluts)	((nluts) * (AVR_CCL_LUT_INPUTS + 1))

/*
 * Canonical event/peripheral input sources cached by the model. The INSEL
 * field of each LUT input selects one of these (family-dependent encoding);
 * a producer drives the corresponding source IRQ (base + id) to set its level.
 * tinyAVR-1 (DS40002205A p.413-415 / DS40002204A p.416) and megaAVR-0
 * (DS40002172C/74C/73C p.372-373) map INSEL values onto these differently —
 * see ccl_src_for_insel() in avr_ccl.c. Sources not fitted on a given device
 * are simply never driven, so they read 0 (matching the datasheet "Reserved").
 */
enum avr_ccl_src {
	AVR_CCL_SRC_EVENT0 = 0,	/* tiny EVENT0 / mega EVENTA */
	AVR_CCL_SRC_EVENT1,	/* tiny EVENT1 / mega EVENTB */
	AVR_CCL_SRC_AC0,
	AVR_CCL_SRC_AC1,	/* tinyAVR-1 3-AC parts only */
	AVR_CCL_SRC_AC2,	/* tinyAVR-1 3-AC parts only */
	AVR_CCL_SRC_TCB0,
	AVR_CCL_SRC_TCB1,
	AVR_CCL_SRC_TCB2,	/* megaAVR-0 only */
	AVR_CCL_SRC_TCA0_WO0,
	AVR_CCL_SRC_TCA0_WO1,
	AVR_CCL_SRC_TCA0_WO2,
	AVR_CCL_SRC_TCD0_WOA,	/* tinyAVR-1 only */
	AVR_CCL_SRC_TCD0_WOB,	/* tinyAVR-1 only */
	AVR_CCL_SRC_USART0_XCK,	/* tinyAVR-1 IN0 of USART src */
	AVR_CCL_SRC_USART0_TXD,
	AVR_CCL_SRC_USART1_TXD,	/* megaAVR-0 only */
	AVR_CCL_SRC_USART2_TXD,	/* megaAVR-0 only */
	AVR_CCL_SRC_SPI0_SCK,
	AVR_CCL_SRC_SPI0_MOSI,
	AVR_CCL_SRC_SPI0_MISO,
	AVR_CCL_NSRC,
};

/* Source IRQ index for source 'src' on a 2-LUT / 4-LUT block. */
#define AVR_CCL_IRQ_SRC_2LUT(src)	(AVR_CCL_LUT_IRQS(2) + (src))
#define AVR_CCL_IRQ_SRC_4LUT(src)	(AVR_CCL_LUT_IRQS(4) + (src))

typedef struct avr_ccl_t {
	avr_io_t	io;
	char		name;		/* '0', … */

	avr_io_addr_t	base;
	avr_io_addr_t	r_ctrla;
	avr_io_addr_t	r_seqctrl[2];
	uint8_t		nluts;
	uint8_t		nseq;
	uint8_t		lut_offset;
	uint8_t		lut_stride;
	uint8_t		clksrc_mask;
	uint8_t		clksrc_shift;
	uint8_t		is_mega;	/* selects the INSEL source-decode table */

	uint8_t		direct[AVR_CCL_MAX_LUTS];	/* direct LUT truth output */
	uint8_t		filtered[AVR_CCL_MAX_LUTS];	/* synchronizer/filter output */
	uint8_t		edge[AVR_CCL_MAX_LUTS];		/* one-clock rising-edge pulse */
	uint8_t		pub[AVR_CCL_MAX_LUTS];		/* last value published on OUT IRQ */
	uint8_t		seq[AVR_CCL_MAX_LUTS / 2];	/* sequencer state per LUT pair */
	uint8_t		io_in[AVR_CCL_MAX_LUTS][AVR_CCL_LUT_INPUTS];	/* IO-source levels */
	uint8_t		src_level[AVR_CCL_NSRC];	/* cached event/peripheral source levels */
	uint8_t		hist[AVR_CCL_MAX_LUTS][4];	/* clocked filter history */
	uint8_t		prev_filtered[AVR_CCL_MAX_LUTS];
	uint8_t		clock_level[AVR_CCL_MAX_LUTS];
	uint8_t		timer_running[AVR_CCL_MAX_LUTS];
	int			base_irq;
	struct {
		void * ccl;
		uint8_t lut;
	} tick_ctx[AVR_CCL_MAX_LUTS];
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

void
avr_ccl_init_mega(
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
