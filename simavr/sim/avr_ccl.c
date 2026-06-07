/*
	avr_ccl.c

	"Modern" AVR (AVRxt) Configurable Custom Logic.
	See avr_ccl.h.

	Each LUT owns four registers (LUTnCTRLA/B/C, TRUTHn). The direct LUT output
	is bit ((in2<<2)|(in1<<1)|in0) of TRUTHn, where each input is routed by the
	INSEL fields. LINK uses the next LUT's direct output; FEEDBACK uses the pair
	sequencer output; the event/peripheral INSEL values are decoded per family
	(ccl_src_for_insel) and read from cached source levels. Optional
	synchronizer/filter, edge detector, and sequencer stages are clocked either
	by CLK_PER (modelled as one simulator cycle per tick) or by the routed IN2
	signal when CLKSRC selects that source.

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

/*
 * Map an INSEL value + input position (i = 0,1,2) to a canonical source id, or
 * -1 if the value selects no event/peripheral source on this family (reserved,
 * or one of MASK/FEEDBACK/LINK/IO handled by the caller). The two families use
 * different value->source maps; both verified against the datasheets:
 *
 *  tinyAVR-1 (ATtiny3216/17 DS40002205A p.413-415, ATtiny1614/16/17
 *  DS40002204A p.416, ATtiny417/814/816/817 DS40002288A p.417 — identical IP;
 *  smaller parts are a strict subset so unfitted AC1/AC2/TCB1 read 0):
 *    0x6 AC0 | 0x7 TCB0 | 0x8 TCA0 WO0/WO1/WO2 | 0x9 TCD0 WOA/WOB/WOA
 *    0xA USART0 XCK/TXD/(IN2 reserved) | 0xB SPI0 SCK/MOSI/MISO
 *    0xC AC1 | 0xD TCB1 | 0xE AC2
 *
 *  megaAVR-0 (ATmega808/809/1608/09 DS40002172C, 3208/09 DS40002174C,
 *  4808/09 DS40002173C — all p.372-373; CCL only reaches USART0-2 and TCB0-2
 *  even on the 4-USART/4-TCB parts):
 *    0x6 AC0 | 0x8 USART0/1/2 TXD | 0x9 SPI0 MOSI/MOSI/SCK
 *    0xA TCA0 WO0/WO1/WO2 | 0xC TCB0/1/2 WO  (0x7, 0xB reserved)
 *
 *  0x3/0x4 select the two event inputs on both (tiny EVENT0/1, mega EVENTA/B).
 */
static int ccl_src_for_insel(avr_ccl_t *p, uint8_t insel, int i)
{
	if (insel == 0x3)
		return AVR_CCL_SRC_EVENT0;
	if (insel == 0x4)
		return AVR_CCL_SRC_EVENT1;
	if (!p->is_mega) {
		switch (insel) {
		case 0x6: return AVR_CCL_SRC_AC0;
		case 0x7: return AVR_CCL_SRC_TCB0;
		case 0x8: return AVR_CCL_SRC_TCA0_WO0 + i;
		case 0x9: return i == 1 ? AVR_CCL_SRC_TCD0_WOB : AVR_CCL_SRC_TCD0_WOA;
		case 0xA: return i == 0 ? AVR_CCL_SRC_USART0_XCK :
				 i == 1 ? AVR_CCL_SRC_USART0_TXD : -1;
		case 0xB: return i == 0 ? AVR_CCL_SRC_SPI0_SCK :
				 i == 1 ? AVR_CCL_SRC_SPI0_MOSI : AVR_CCL_SRC_SPI0_MISO;
		case 0xC: return AVR_CCL_SRC_AC1;
		case 0xD: return AVR_CCL_SRC_TCB1;
		case 0xE: return AVR_CCL_SRC_AC2;
		default:  return -1;
		}
	}
	switch (insel) {
	case 0x6: return AVR_CCL_SRC_AC0;
	case 0x8: return AVR_CCL_SRC_USART0_TXD + i;	/* USART0/1/2 TXD */
	case 0x9: return i == 2 ? AVR_CCL_SRC_SPI0_SCK : AVR_CCL_SRC_SPI0_MOSI;
	case 0xA: return AVR_CCL_SRC_TCA0_WO0 + i;
	case 0xC: return AVR_CCL_SRC_TCB0 + i;		/* TCB0/1/2 WO */
	default:  return -1;
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
	default: {
		int src = ccl_src_for_insel(p, lut_insel(p, n, i), i);
		return src < 0 ? 0 : (p->src_level[src] & 1);
	}
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

/*
 * A producer (event channel, comparator, timer waveform, …) drives one of the
 * shared event/peripheral source levels. The core wires real peripheral output
 * IRQs to these; a board or test can also drive them directly.
 */
static void
avr_ccl_src_input(struct avr_irq_t *irq, uint32_t value, void *param)
{
	avr_ccl_t *p = (avr_ccl_t *)param;
	int s = irq->irq - p->base_irq - AVR_CCL_LUT_IRQS(p->nluts);
	if (s < 0 || s >= AVR_CCL_NSRC)
		return;
	if (p->src_level[s] == (value & 1))
		return;
	p->src_level[s] = value & 1;
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
	memset(p->src_level, 0, sizeof(p->src_level));
	memset(p->hist, 0, sizeof(p->hist));
	memset(p->prev_filtered, 0, sizeof(p->prev_filtered));
	memset(p->clock_level, 0, sizeof(p->clock_level));
	for (int n = 0; n < p->nluts; n++) {
		p->timer_running[n] = 0;
		avr_cycle_timer_cancel(p->io.avr, avr_ccl_tick, &p->tick_ctx[n]);
	}
}

/* Shared source-IRQ names, indexed by avr_ccl_src (appended after the per-LUT
 * IRQs at AVR_CCL_LUT_IRQS(nluts)). */
#define CCL_SRC_NAMES(base) \
	[(base) + AVR_CCL_SRC_EVENT0] = "ccl.ev0", \
	[(base) + AVR_CCL_SRC_EVENT1] = "ccl.ev1", \
	[(base) + AVR_CCL_SRC_AC0] = "ccl.ac0", \
	[(base) + AVR_CCL_SRC_AC1] = "ccl.ac1", \
	[(base) + AVR_CCL_SRC_AC2] = "ccl.ac2", \
	[(base) + AVR_CCL_SRC_TCB0] = "ccl.tcb0", \
	[(base) + AVR_CCL_SRC_TCB1] = "ccl.tcb1", \
	[(base) + AVR_CCL_SRC_TCB2] = "ccl.tcb2", \
	[(base) + AVR_CCL_SRC_TCA0_WO0] = "ccl.tca0wo0", \
	[(base) + AVR_CCL_SRC_TCA0_WO1] = "ccl.tca0wo1", \
	[(base) + AVR_CCL_SRC_TCA0_WO2] = "ccl.tca0wo2", \
	[(base) + AVR_CCL_SRC_TCD0_WOA] = "ccl.tcd0woa", \
	[(base) + AVR_CCL_SRC_TCD0_WOB] = "ccl.tcd0wob", \
	[(base) + AVR_CCL_SRC_USART0_XCK] = "ccl.us0xck", \
	[(base) + AVR_CCL_SRC_USART0_TXD] = "ccl.us0txd", \
	[(base) + AVR_CCL_SRC_USART1_TXD] = "ccl.us1txd", \
	[(base) + AVR_CCL_SRC_USART2_TXD] = "ccl.us2txd", \
	[(base) + AVR_CCL_SRC_SPI0_SCK] = "ccl.spi0sck", \
	[(base) + AVR_CCL_SRC_SPI0_MOSI] = "ccl.spi0mosi", \
	[(base) + AVR_CCL_SRC_SPI0_MISO] = "ccl.spi0miso"

static const char *irq_names_2lut[AVR_CCL_LUT_IRQS(2) + AVR_CCL_NSRC] = {
	[AVR_CCL_IRQ_LUT0_IN0] = "ccl.l0in0", [AVR_CCL_IRQ_LUT0_IN1] = "ccl.l0in1",
	[AVR_CCL_IRQ_LUT0_IN2] = "ccl.l0in2", [AVR_CCL_IRQ_LUT1_IN0] = "ccl.l1in0",
	[AVR_CCL_IRQ_LUT1_IN1] = "ccl.l1in1", [AVR_CCL_IRQ_LUT1_IN2] = "ccl.l1in2",
	[AVR_CCL_IRQ_LUT0_OUT] = ">ccl.l0out", [AVR_CCL_IRQ_LUT1_OUT] = ">ccl.l1out",
	CCL_SRC_NAMES(AVR_CCL_LUT_IRQS(2)),
};

static const char *irq_names_4lut[AVR_CCL_LUT_IRQS(4) + AVR_CCL_NSRC] = {
	[0] = "ccl.l0in0", [1] = "ccl.l0in1", [2] = "ccl.l0in2",
	[3] = "ccl.l1in0", [4] = "ccl.l1in1", [5] = "ccl.l1in2",
	[6] = "ccl.l2in0", [7] = "ccl.l2in1", [8] = "ccl.l2in2",
	[9] = "ccl.l3in0", [10] = "ccl.l3in1", [11] = "ccl.l3in2",
	[12] = ">ccl.l0out", [13] = ">ccl.l1out",
	[14] = ">ccl.l2out", [15] = ">ccl.l3out",
	CCL_SRC_NAMES(AVR_CCL_LUT_IRQS(4)),
};

static avr_io_t _io = {
	.kind = "ccl",
	.reset = avr_ccl_reset,
	.irq_names = NULL,
};

/*
 * Common init. The per-LUT register layout and the CLKSRC field differ between
 * families, so the layout fields must be fully resolved *before* the per-LUT
 * write handlers are registered — otherwise the hooks land on the wrong
 * addresses. Verified register maps:
 *
 *   tinyAVR 1-series (ATtiny3217 DS40002205A p.409, ATtiny161x DS40002021C):
 *     SEQCTRL0 @0x01; LUT0CTRLA @0x05, stride 4;
 *     LUTnCTRLA = [7]EDGEDET [6]CLKSRC(1b) [5:4]FILTSEL [3]OUTEN [0]ENABLE.
 *   megaAVR 0-series (ATmega3208/09 DS40002174C p.381; same IP as ATmega4809):
 *     SEQCTRL0 @0x01, SEQCTRL1 @0x02; LUT0CTRLA @0x08, stride 4 (LUT2 @0x10);
 *     LUTnCTRLA = [7]EDGEDET [6]OUTEN [5:4]FILTSEL [3:1]CLKSRC(3b) [0]ENABLE.
 *
 * EDGEDET (0x80), FILTSEL (0x30) and ENABLE (0x01) share positions across both;
 * only CLKSRC moves, so it is the one field parameterised per family. OUTEN is
 * not consumed (the LUT output is always published on its IRQ).
 */
static void
ccl_init_common(
		avr_t * avr,
		avr_ccl_t * p,
		avr_io_addr_t base,
		uint8_t nluts,
		char name,
		int is_mega)
{
	memset(p, 0, sizeof(*p));
	p->io = _io;
	p->name = name;
	p->base = base;
	p->r_ctrla = base + CCLR_CTRLA;
	p->r_seqctrl[0] = base + CCLR_SEQCTRL0;
	p->nluts = nluts > AVR_CCL_MAX_LUTS ? AVR_CCL_MAX_LUTS : nluts;
	p->lut_stride = 4;
	p->is_mega = is_mega;
	if (is_mega) {
		p->r_seqctrl[1] = base + 0x02;
		p->nseq = 2;
		p->lut_offset = 0x08;
		p->clksrc_mask = 0x0e;
		p->clksrc_shift = 1;
	} else {
		p->nseq = 1;
		p->lut_offset = CCLR_LUT0CTRLA;
		p->clksrc_mask = 0x40;
		p->clksrc_shift = 6;
	}
	p->io.irq_names = p->nluts > 2 ? irq_names_4lut : irq_names_2lut;
	for (int n = 0; n < p->nluts; n++) {
		avr_ccl_tick_ctx_t *ctx = (avr_ccl_tick_ctx_t *)&p->tick_ctx[n];
		ctx->ccl = p;
		ctx->lut = n;
	}

	/* inputs + one output each, then the shared event/peripheral sources */
	int nirq = AVR_CCL_LUT_IRQS(p->nluts) + AVR_CCL_NSRC;

	avr_register_io(avr, &p->io);
	avr_io_setirqs(&p->io, AVR_IOCTL_CCL_GETIRQ(name), nirq, NULL);
	p->base_irq = p->io.irq[0].irq;
	for (int i = 0; i < p->nluts * AVR_CCL_LUT_INPUTS; i++)
		avr_irq_register_notify(p->io.irq + i, avr_ccl_irq_input, p);
	for (int s = 0; s < AVR_CCL_NSRC; s++)
		avr_irq_register_notify(p->io.irq + AVR_CCL_LUT_IRQS(p->nluts) + s,
								avr_ccl_src_input, p);

	/* Any config write re-evaluates the logic. */
	avr_register_io_write(avr, p->r_ctrla, avr_ccl_reg_write, p);
	for (int seq = 0; seq < p->nseq; seq++)
		avr_register_io_write(avr, p->r_seqctrl[seq], avr_ccl_reg_write, p);
	for (int n = 0; n < p->nluts; n++) {
		avr_register_io_write(avr, lut_ctrla(p, n), avr_ccl_reg_write, p);
		avr_register_io_write(avr, lut_ctrlb(p, n), avr_ccl_reg_write, p);
		avr_register_io_write(avr, lut_ctrlc(p, n), avr_ccl_reg_write, p);
		avr_register_io_write(avr, lut_truth(p, n), avr_ccl_reg_write, p);
	}
}

void
avr_ccl_init(
		avr_t * avr,
		avr_ccl_t * p,
		avr_io_addr_t base,
		uint8_t nluts,
		char name)
{
	ccl_init_common(avr, p, base, nluts, name, 0);
}

void
avr_ccl_init_mega(
		avr_t * avr,
		avr_ccl_t * p,
		avr_io_addr_t base,
		uint8_t nluts,
		char name)
{
	ccl_init_common(avr, p, base, nluts, name, 1);
}
