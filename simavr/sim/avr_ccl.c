/*
	avr_ccl.c

	"Modern" AVR (AVRxt) Configurable Custom Logic — combinational LUT core.
	See avr_ccl.h.

	Each LUT n owns four registers (LUTnCTRLA/B/C, TRUTHn) at base+0x05+n*4.
	Its output is bit ((in2<<2)|(in1<<1)|in0) of TRUTHn, where each input is
	routed by the INSEL fields. The whole CCL is re-evaluated on any config or
	IO-input change; because LINK/FEEDBACK route LUT outputs back as inputs, the
	evaluation iterates to a fixed point (bounded) before publishing the OUT IRQs.

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

#include <stdio.h>
#include <string.h>
#include "avr_ccl.h"

/* CTRLA */
#define ENABLE_bm	0x01

/* LUTnCTRLA */
#define LUT_ENABLE_bm	0x01

/* INSEL source values (LUTnCTRLB/C) */
#define INSEL_MASK	0x0
#define INSEL_FEEDBACK	0x1
#define INSEL_LINK	0x2
#define INSEL_IO	0x5

static inline uint8_t rd(avr_t *avr, avr_io_addr_t a) { return avr->data[a]; }

/* Per-LUT register addresses (block stride 4, first block at base+0x05). */
static avr_io_addr_t lut_ctrla(avr_ccl_t *p, int n) { return p->r_ctrla + 5 + n * 4; }
static avr_io_addr_t lut_ctrlb(avr_ccl_t *p, int n) { return lut_ctrla(p, n) + 1; }
static avr_io_addr_t lut_ctrlc(avr_ccl_t *p, int n) { return lut_ctrla(p, n) + 2; }
static avr_io_addr_t lut_truth(avr_ccl_t *p, int n) { return lut_ctrla(p, n) + 3; }

static uint8_t lut_insel(avr_ccl_t *p, int n, int i)
{
	avr_t *avr = p->io.avr;
	switch (i) {
	case 0: return rd(avr, lut_ctrlb(p, n)) & 0x0f;
	case 1: return (rd(avr, lut_ctrlb(p, n)) >> 4) & 0x0f;
	default: return rd(avr, lut_ctrlc(p, n)) & 0x0f;	/* i == 2 */
	}
}

/* Resolve one routed LUT input to 0/1 using the currently-committed outputs. */
static uint8_t lut_input(avr_ccl_t *p, int n, int i)
{
	switch (lut_insel(p, n, i)) {
	case INSEL_MASK:	return 0;
	case INSEL_FEEDBACK:	return p->out[n];
	case INSEL_LINK:	return p->out[(n + 1) % p->nluts];
	case INSEL_IO:		return p->io_in[n][i] & 1;
	default:		return 0;	/* events/peripherals not modelled */
	}
}

static uint8_t lut_compute(avr_ccl_t *p, int n)
{
	avr_t *avr = p->io.avr;
	if (!(rd(avr, p->r_ctrla) & ENABLE_bm) ||
		!(rd(avr, lut_ctrla(p, n)) & LUT_ENABLE_bm))
		return 0;
	uint8_t idx = (lut_input(p, n, 2) << 2) |
				  (lut_input(p, n, 1) << 1) |
				   lut_input(p, n, 0);
	return (rd(avr, lut_truth(p, n)) >> idx) & 1;
}

/* Re-evaluate every LUT to a fixed point, then publish changed OUT IRQs. */
static void ccl_eval(avr_ccl_t *p)
{
	/* Bounded iteration: LINK/FEEDBACK can chain, so settle (or stop if it
	 * oscillates — real CCL combinational loops are a user error). */
	for (int pass = 0; pass < 2 * AVR_CCL_MAX_LUTS; pass++) {
		int changed = 0;
		for (int n = 0; n < p->nluts; n++) {
			uint8_t v = lut_compute(p, n);
			if (v != p->out[n]) {
				p->out[n] = v;
				changed = 1;
			}
		}
		if (!changed)
			break;
	}

	for (int n = 0; n < p->nluts; n++)
		if (p->out[n] != p->pub[n]) {
			p->pub[n] = p->out[n];
			avr_raise_irq(p->io.irq + p->nluts * AVR_CCL_LUT_INPUTS + n,
						  p->out[n]);
		}
}

static void
avr_ccl_reg_write(struct avr_t *avr, avr_io_addr_t addr, uint8_t v, void *param)
{
	avr_ccl_t *p = (avr_ccl_t *)param;
	avr_core_watch_write(avr, addr, v);
	ccl_eval(p);
}

/* A board/test drives an external level on a LUTn-INm pin. */
static void
avr_ccl_irq_input(struct avr_irq_t *irq, uint32_t value, void *param)
{
	avr_ccl_t *p = (avr_ccl_t *)param;
	int idx = irq->irq - p->base_irq;
	if (idx < 0 || idx >= p->nluts * AVR_CCL_LUT_INPUTS)
		return;
	p->io_in[idx / AVR_CCL_LUT_INPUTS][idx % AVR_CCL_LUT_INPUTS] = value & 1;
	ccl_eval(p);
}

static void
avr_ccl_reset(avr_io_t *io)
{
	avr_ccl_t *p = (avr_ccl_t *)io;
	memset(p->out, 0, sizeof(p->out));
	memset(p->pub, 0, sizeof(p->pub));
	memset(p->io_in, 0, sizeof(p->io_in));
}

static const char *irq_names[AVR_CCL_IRQ_COUNT_2LUT] = {
	[AVR_CCL_IRQ_LUT0_IN0] = "ccl.l0in0", [AVR_CCL_IRQ_LUT0_IN1] = "ccl.l0in1",
	[AVR_CCL_IRQ_LUT0_IN2] = "ccl.l0in2", [AVR_CCL_IRQ_LUT1_IN0] = "ccl.l1in0",
	[AVR_CCL_IRQ_LUT1_IN1] = "ccl.l1in1", [AVR_CCL_IRQ_LUT1_IN2] = "ccl.l1in2",
	[AVR_CCL_IRQ_LUT0_OUT] = ">ccl.l0out", [AVR_CCL_IRQ_LUT1_OUT] = ">ccl.l1out",
};

static avr_io_t _io = {
	.kind = "ccl",
	.reset = avr_ccl_reset,
	.irq_names = irq_names,
};

void
avr_ccl_init(
		avr_t * avr,
		avr_ccl_t * p,
		avr_io_addr_t base,
		uint8_t nluts,
		char name)
{
	memset(p, 0, sizeof(*p));
	p->io = _io;
	p->name = name;
	p->base = base;
	p->r_ctrla = base + CCLR_CTRLA;
	p->nluts = nluts > AVR_CCL_MAX_LUTS ? AVR_CCL_MAX_LUTS : nluts;

	int nirq = p->nluts * (AVR_CCL_LUT_INPUTS + 1);	/* inputs + one output each */

	avr_register_io(avr, &p->io);
	avr_io_setirqs(&p->io, AVR_IOCTL_CCL_GETIRQ(name), nirq, NULL);
	p->base_irq = p->io.irq[0].irq;
	for (int i = 0; i < p->nluts * AVR_CCL_LUT_INPUTS; i++)
		avr_irq_register_notify(p->io.irq + i, avr_ccl_irq_input, p);

	/* Any config write re-evaluates the logic. */
	avr_register_io_write(avr, p->r_ctrla, avr_ccl_reg_write, p);
	for (int n = 0; n < p->nluts; n++) {
		avr_register_io_write(avr, lut_ctrla(p, n), avr_ccl_reg_write, p);
		avr_register_io_write(avr, lut_ctrlb(p, n), avr_ccl_reg_write, p);
		avr_register_io_write(avr, lut_ctrlc(p, n), avr_ccl_reg_write, p);
		avr_register_io_write(avr, lut_truth(p, n), avr_ccl_reg_write, p);
	}
}
