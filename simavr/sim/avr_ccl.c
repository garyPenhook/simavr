/*
	avr_ccl.c

	"Modern" AVR (AVRxt) Configurable Custom Logic.
	See avr_ccl.h.

	Each LUT owns four registers (LUTnCTRLA/B/C, TRUTHn). The direct LUT output
	is bit ((in2<<2)|(in1<<1)|in0) of TRUTHn, where each input is routed by the
	INSEL fields. LINK uses the next LUT's direct output; FEEDBACK uses the pair
	sequencer output. Optional synchronizer/filter, edge detector, and sequencer
	stages are clocked either by CLK_PER (modelled as one simulator cycle per
	tick) or by the routed IN2 signal when CLKSRC selects that source.

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
#include "sim_cycle_timers.h"

/* CTRLA */
#define ENABLE_bm	0x01

/* LUTnCTRLA */
#define LUT_ENABLE_bm	0x01
#define FILTSEL_gm	0x30
#define FILTSEL_gp	4
#define EDGEDET_bm	0x80

/* SEQCTRLn */
#define SEQSEL_gm	0x07

/* INSEL source values (LUTnCTRLB/C) */
#define INSEL_MASK	0x0
#define INSEL_FEEDBACK	0x1
#define INSEL_LINK	0x2
#define INSEL_IO	0x5

enum {
	FILTSEL_DISABLE = 0,
	FILTSEL_SYNCH = 1,
	FILTSEL_FILTER = 2,
};

enum {
	SEQSEL_DISABLE = 0,
	SEQSEL_DFF = 1,
	SEQSEL_JK = 2,
	SEQSEL_LATCH = 3,
	SEQSEL_RS = 4,
};

typedef struct avr_ccl_tick_ctx_t {
	avr_ccl_t * ccl;
	uint8_t lut;
} avr_ccl_tick_ctx_t;

static inline uint8_t rd(avr_t *avr, avr_io_addr_t a) { return avr->data[a]; }

/* Per-LUT register addresses (layout is family-dependent). */
static avr_io_addr_t lut_ctrla(avr_ccl_t *p, int n)
{
	return p->base + p->lut_offset + n * p->lut_stride;
}
static avr_io_addr_t lut_ctrlb(avr_ccl_t *p, int n) { return lut_ctrla(p, n) + 1; }
static avr_io_addr_t lut_ctrlc(avr_ccl_t *p, int n) { return lut_ctrla(p, n) + 2; }
static avr_io_addr_t lut_truth(avr_ccl_t *p, int n) { return lut_ctrla(p, n) + 3; }

static inline int seq_index_for_lut(int n) { return n / 2; }
static inline int seq_even_lut(int seq) { return seq * 2; }

static uint8_t seq_mode(avr_ccl_t *p, int seq)
{
	if (seq < 0 || seq >= p->nseq)
		return SEQSEL_DISABLE;
	return rd(p->io.avr, p->r_seqctrl[seq]) & SEQSEL_gm;
}

static uint8_t lut_filtsel(avr_ccl_t *p, int n)
{
	return (rd(p->io.avr, lut_ctrla(p, n)) & FILTSEL_gm) >> FILTSEL_gp;
}

static uint8_t lut_edge_enabled(avr_ccl_t *p, int n)
{
	return !!(rd(p->io.avr, lut_ctrla(p, n)) & EDGEDET_bm);
}

static uint8_t lut_clock_source(avr_ccl_t *p, int n)
{
	return (rd(p->io.avr, lut_ctrla(p, n)) & p->clksrc_mask) >> p->clksrc_shift;
}

static uint8_t lut_clock_uses_input2(avr_ccl_t *p, int n)
{
	return lut_clock_source(p, n) == 1;
}

static uint8_t lut_insel(avr_ccl_t *p, int n, int i)
{
	avr_t *avr = p->io.avr;
	switch (i) {
	case 0: return rd(avr, lut_ctrlb(p, n)) & 0x0f;
	case 1: return (rd(avr, lut_ctrlb(p, n)) >> 4) & 0x0f;
	default: return rd(avr, lut_ctrlc(p, n)) & 0x0f;	/* i == 2 */
	}
}

/* Resolve one routed LUT input to 0/1 using the current direct/seq outputs. */
static uint8_t lut_input(avr_ccl_t *p, int n, int i, int truth_path)
{
	if (truth_path && i == 2 && lut_clock_uses_input2(p, n))
		return 0;	/* IN2 becomes the alternate clock source */
	switch (lut_insel(p, n, i)) {
	case INSEL_MASK:	return 0;
	case INSEL_FEEDBACK:	return p->seq[seq_index_for_lut(n)] & 1;
	case INSEL_LINK:	return p->direct[(n + 1) % p->nluts] & 1;
	case INSEL_IO:		return p->io_in[n][i] & 1;
	default:		return 0;	/* events/peripherals not modelled */
	}
}

static uint8_t lut_direct_compute(avr_ccl_t *p, int n)
{
	avr_t *avr = p->io.avr;
	if (!(rd(avr, p->r_ctrla) & ENABLE_bm) ||
		!(rd(avr, lut_ctrla(p, n)) & LUT_ENABLE_bm))
		return 0;
	uint8_t idx = (lut_input(p, n, 2, 1) << 2) |
				  (lut_input(p, n, 1, 1) << 1) |
				   lut_input(p, n, 0, 1);
	return (rd(avr, lut_truth(p, n)) >> idx) & 1;
}

static uint8_t lut_visible_output(avr_ccl_t *p, int n)
{
	uint8_t filt = lut_filtsel(p, n);
	if (lut_edge_enabled(p, n) && filt != FILTSEL_DISABLE)
		return p->edge[n] & 1;
	if (filt != FILTSEL_DISABLE)
		return p->filtered[n] & 1;
	return p->direct[n] & 1;
}

static void ccl_publish(avr_ccl_t *p)
{
	for (int n = 0; n < p->nluts; n++) {
		uint8_t v = lut_visible_output(p, n);
		if (v != p->pub[n]) {
			p->pub[n] = v;
			avr_raise_irq(p->io.irq + p->nluts * AVR_CCL_LUT_INPUTS + n, v);
		}
	}
}

static void ccl_eval_direct(avr_ccl_t *p)
{
	/* Bounded iteration: LINK/FEEDBACK can chain, so settle (or stop if it
	 * oscillates — real CCL combinational loops are a user error). */
	for (int pass = 0; pass < 2 * AVR_CCL_MAX_LUTS; pass++) {
		int changed = 0;
		for (int n = 0; n < p->nluts; n++) {
			uint8_t v = lut_direct_compute(p, n);
			if (v != p->direct[n]) {
				p->direct[n] = v;
				changed = 1;
			}
		}
		if (!changed)
			break;
	}
}

static void ccl_reset_clocked_state(avr_ccl_t *p, int n)
{
	memset(p->hist[n], 0, sizeof(p->hist[n]));
	p->filtered[n] = 0;
	p->edge[n] = 0;
	p->prev_filtered[n] = 0;
	p->clock_level[n] = 0;
}

static void ccl_reset_seq(avr_ccl_t *p, int seq)
{
	if (seq >= 0 && seq < p->nseq)
		p->seq[seq] = 0;
}

static void ccl_clock_lut(avr_ccl_t *p, int n)
{
	uint8_t filt = lut_filtsel(p, n);

	if (!(rd(p->io.avr, p->r_ctrla) & ENABLE_bm) ||
		!(rd(p->io.avr, lut_ctrla(p, n)) & LUT_ENABLE_bm)) {
		ccl_reset_clocked_state(p, n);
		return;
	}

	for (int i = 3; i > 0; i--)
		p->hist[n][i] = p->hist[n][i - 1];
	p->hist[n][0] = p->direct[n] & 1;

	switch (filt) {
	case FILTSEL_SYNCH:
		p->filtered[n] = p->hist[n][1];
		break;
	case FILTSEL_FILTER:
		if (p->hist[n][1] == p->hist[n][2] &&
			p->hist[n][2] == p->hist[n][3])
			p->filtered[n] = p->hist[n][3];
		break;
	default:
		p->filtered[n] = p->direct[n] & 1;
		break;
	}

	if (lut_edge_enabled(p, n) && filt != FILTSEL_DISABLE) {
		uint8_t pulse = (!p->prev_filtered[n] && p->filtered[n]) ? 1 : 0;
		p->edge[n] = pulse;
		p->prev_filtered[n] = p->filtered[n];
	} else {
		p->edge[n] = 0;
		p->prev_filtered[n] = p->filtered[n];
	}
}

static void ccl_clock_seq(avr_ccl_t *p, int seq)
{
	int even = seq_even_lut(seq);
	int odd = even + 1;
	uint8_t mode = seq_mode(p, seq);
	uint8_t q = p->seq[seq];
	uint8_t d, g;

	if (mode == SEQSEL_DISABLE || even >= p->nluts)
		return;
	if (!(rd(p->io.avr, p->r_ctrla) & ENABLE_bm) ||
		!(rd(p->io.avr, lut_ctrla(p, even)) & LUT_ENABLE_bm)) {
		p->seq[seq] = 0;
		return;
	}

	d = lut_visible_output(p, even);
	g = odd < p->nluts ? lut_visible_output(p, odd) : 0;

	switch (mode) {
	case SEQSEL_DFF:
		if (g)
			q = d;
		break;
	case SEQSEL_JK:
		if (d && g)
			q ^= 1;
		else if (d)
			q = 1;
		else if (g)
			q = 0;
		break;
	case SEQSEL_LATCH:
		if (g)
			q = d;
		break;
	case SEQSEL_RS:
		if (d && !g)
			q = 1;
		else if (!d && g)
			q = 0;
		break;
	default:
		break;
	}
	p->seq[seq] = q & 1;
}

static void ccl_clock_path(avr_ccl_t *p, int n)
{
	ccl_clock_lut(p, n);
	if ((n & 1) == 0) {
		ccl_clock_seq(p, seq_index_for_lut(n));
		ccl_eval_direct(p);	/* FEEDBACK can immediately affect LUT outputs */
	}
	ccl_publish(p);
}

static avr_cycle_count_t
avr_ccl_tick(struct avr_t *avr, avr_cycle_count_t when, void *param)
{
	avr_ccl_tick_ctx_t *ctx = param;
	avr_ccl_t *p = ctx->ccl;
	int n = ctx->lut;

	(void)when;
	if (!p->timer_running[n])
		return 0;
	if (!(rd(avr, p->r_ctrla) & ENABLE_bm) ||
		!(rd(avr, lut_ctrla(p, n)) & LUT_ENABLE_bm) ||
		lut_clock_uses_input2(p, n)) {
		p->timer_running[n] = 0;
		return 0;
	}
	ccl_eval_direct(p);
	ccl_clock_path(p, n);
	return avr->cycle + 1;
}

static void ccl_update_timers(avr_ccl_t *p)
{
	for (int n = 0; n < p->nluts; n++) {
		int want = 0;
		if ((rd(p->io.avr, p->r_ctrla) & ENABLE_bm) &&
			(rd(p->io.avr, lut_ctrla(p, n)) & LUT_ENABLE_bm) &&
			!lut_clock_uses_input2(p, n)) {
			want = lut_filtsel(p, n) != FILTSEL_DISABLE || lut_edge_enabled(p, n);
			if (!want && (n & 1) == 0)
				want = seq_mode(p, seq_index_for_lut(n)) != SEQSEL_DISABLE;
		}
		if (want && !p->timer_running[n]) {
			p->timer_running[n] = 1;
			avr_cycle_timer_register(p->io.avr, 1, avr_ccl_tick, &p->tick_ctx[n]);
		} else if (!want && p->timer_running[n]) {
			p->timer_running[n] = 0;
			avr_cycle_timer_cancel(p->io.avr, avr_ccl_tick, &p->tick_ctx[n]);
		}
	}
}

/* Re-evaluate the direct LUT logic, clock any IN2-sourced stateful paths, then
 * publish the current visible outputs. */
static void ccl_eval(avr_ccl_t *p)
{
	ccl_eval_direct(p);

	for (int n = 0; n < p->nluts; n++) {
		uint8_t clk = lut_input(p, n, 2, 0) & 1;
		if (lut_clock_uses_input2(p, n) && !p->clock_level[n] && clk)
			ccl_clock_path(p, n);
		p->clock_level[n] = clk;
	}

	for (int seq = 0; seq < p->nseq; seq++) {
		int even = seq_even_lut(seq);
		if (seq_mode(p, seq) != SEQSEL_DISABLE &&
			(!(rd(p->io.avr, p->r_ctrla) & ENABLE_bm) ||
			 !(rd(p->io.avr, lut_ctrla(p, even)) & LUT_ENABLE_bm)))
			ccl_reset_seq(p, seq);
	}

	ccl_update_timers(p);
	ccl_publish(p);
}

static void
avr_ccl_reg_write(struct avr_t *avr, avr_io_addr_t addr, uint8_t v, void *param)
{
	avr_ccl_t *p = (avr_ccl_t *)param;
	avr_core_watch_write(avr, addr, v);
	if (addr == p->r_ctrla && !(v & ENABLE_bm)) {
		for (int n = 0; n < p->nluts; n++)
			ccl_reset_clocked_state(p, n);
		for (int seq = 0; seq < p->nseq; seq++)
			ccl_reset_seq(p, seq);
	}
	for (int n = 0; n < p->nluts; n++)
		if (addr == lut_ctrla(p, n)) {
			ccl_reset_clocked_state(p, n);
			if ((n & 1) == 0 && !(v & LUT_ENABLE_bm))
				ccl_reset_seq(p, seq_index_for_lut(n));
		}
	for (int seq = 0; seq < p->nseq; seq++)
		if (addr == p->r_seqctrl[seq]) {
			ccl_reset_seq(p, seq);
			ccl_reset_clocked_state(p, seq_even_lut(seq));
			if (seq_even_lut(seq) + 1 < p->nluts)
				ccl_reset_clocked_state(p, seq_even_lut(seq) + 1);
		}
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
	memset(p->direct, 0, sizeof(p->direct));
	memset(p->filtered, 0, sizeof(p->filtered));
	memset(p->edge, 0, sizeof(p->edge));
	memset(p->pub, 0, sizeof(p->pub));
	memset(p->seq, 0, sizeof(p->seq));
	memset(p->io_in, 0, sizeof(p->io_in));
	memset(p->hist, 0, sizeof(p->hist));
	memset(p->prev_filtered, 0, sizeof(p->prev_filtered));
	memset(p->clock_level, 0, sizeof(p->clock_level));
	for (int n = 0; n < p->nluts; n++) {
		p->timer_running[n] = 0;
		avr_cycle_timer_cancel(p->io.avr, avr_ccl_tick, &p->tick_ctx[n]);
	}
}

static const char *irq_names_2lut[AVR_CCL_IRQ_COUNT_2LUT] = {
	[AVR_CCL_IRQ_LUT0_IN0] = "ccl.l0in0", [AVR_CCL_IRQ_LUT0_IN1] = "ccl.l0in1",
	[AVR_CCL_IRQ_LUT0_IN2] = "ccl.l0in2", [AVR_CCL_IRQ_LUT1_IN0] = "ccl.l1in0",
	[AVR_CCL_IRQ_LUT1_IN1] = "ccl.l1in1", [AVR_CCL_IRQ_LUT1_IN2] = "ccl.l1in2",
	[AVR_CCL_IRQ_LUT0_OUT] = ">ccl.l0out", [AVR_CCL_IRQ_LUT1_OUT] = ">ccl.l1out",
};

static const char *irq_names_4lut[AVR_CCL_IRQ_COUNT_4LUT] = {
	[0] = "ccl.l0in0", [1] = "ccl.l0in1", [2] = "ccl.l0in2",
	[3] = "ccl.l1in0", [4] = "ccl.l1in1", [5] = "ccl.l1in2",
	[6] = "ccl.l2in0", [7] = "ccl.l2in1", [8] = "ccl.l2in2",
	[9] = "ccl.l3in0", [10] = "ccl.l3in1", [11] = "ccl.l3in2",
	[12] = ">ccl.l0out", [13] = ">ccl.l1out",
	[14] = ">ccl.l2out", [15] = ">ccl.l3out",
};

static avr_io_t _io = {
	.kind = "ccl",
	.reset = avr_ccl_reset,
	.irq_names = NULL,
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
	p->r_seqctrl[0] = base + CCLR_SEQCTRL0;
	p->nseq = 1;
	p->nluts = nluts > AVR_CCL_MAX_LUTS ? AVR_CCL_MAX_LUTS : nluts;
	p->lut_offset = CCLR_LUT0CTRLA;
	p->lut_stride = 4;
	p->clksrc_mask = 0x40;
	p->clksrc_shift = 6;
	p->io.irq_names = p->nluts > 2 ? irq_names_4lut : irq_names_2lut;
	for (int n = 0; n < p->nluts; n++) {
		avr_ccl_tick_ctx_t *ctx = (avr_ccl_tick_ctx_t *)&p->tick_ctx[n];
		ctx->ccl = p;
		ctx->lut = n;
	}

	int nirq = p->nluts * (AVR_CCL_LUT_INPUTS + 1);	/* inputs + one output each */

	avr_register_io(avr, &p->io);
	avr_io_setirqs(&p->io, AVR_IOCTL_CCL_GETIRQ(name), nirq, NULL);
	p->base_irq = p->io.irq[0].irq;
	for (int i = 0; i < p->nluts * AVR_CCL_LUT_INPUTS; i++)
		avr_irq_register_notify(p->io.irq + i, avr_ccl_irq_input, p);

	/* Any config write re-evaluates the logic. */
	avr_register_io_write(avr, p->r_ctrla, avr_ccl_reg_write, p);
	avr_register_io_write(avr, p->r_seqctrl[0], avr_ccl_reg_write, p);
	for (int n = 0; n < p->nluts; n++) {
		avr_register_io_write(avr, lut_ctrla(p, n), avr_ccl_reg_write, p);
		avr_register_io_write(avr, lut_ctrlb(p, n), avr_ccl_reg_write, p);
		avr_register_io_write(avr, lut_ctrlc(p, n), avr_ccl_reg_write, p);
		avr_register_io_write(avr, lut_truth(p, n), avr_ccl_reg_write, p);
	}
}

void
avr_ccl_init_mega(
		avr_t * avr,
		avr_ccl_t * p,
		avr_io_addr_t base,
		uint8_t nluts,
		char name)
{
	avr_ccl_init(avr, p, base, nluts, name);
	p->r_seqctrl[1] = base + 0x02;
	p->nseq = 2;
	p->lut_offset = 0x08;
	p->lut_stride = 4;
	p->clksrc_mask = 0x0e;
	p->clksrc_shift = 1;
	avr_register_io_write(avr, p->r_seqctrl[1], avr_ccl_reg_write, p);
}
