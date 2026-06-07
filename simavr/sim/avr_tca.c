/*
	avr_tca.c

	"Modern" AVR (AVRxt) 16-bit Timer/Counter type A (TCA). See avr_tca.h.

	Scheduling model
	----------------
	The counter is a prescaled up-counter, 0..TOP (TOP = PER), wrapping to 0.
	Rather than stepping every cycle, a single simavr cycle timer is scheduled to
	the *next* interesting count: the smallest of the three compare values above
	the current count, or the wrap point (TOP+1). On expiry the matching flags
	are set (CMP0/1/2 and/or OVF), the corresponding interrupts are raised, and
	the next event is scheduled. CNT reads compute the live value from elapsed
	cycles.

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
#include "avr_tca.h"
#include "sim_cycle_timers.h"

/* CTRLA */
#define ENABLE_bm	0x01
#define CLKSEL_gm	0x0e
#define CLKSEL_gp	1

/* CTRLB (normal-mode view): WGMODE[2:0], CMPnEN at bits 5/6/7 (DS40002205A
 * 20.5.2). Only single-slope PWM waveform output is modelled here. */
#define WGMODE_gm		0x07
#define WGMODE_SINGLESLOPE	0x03

/* INTCTRL / INTFLAGS */
#define OVF_bm		0x01
#define CMP0_bm		0x10
#define CMP1_bm		0x20
#define CMP2_bm		0x40

static const uint16_t clksel_div[8] = { 1, 2, 4, 8, 16, 64, 256, 1024 };
static const uint8_t cmp_flag[3] = { CMP0_bm, CMP1_bm, CMP2_bm };
static const uint8_t cmpen_bm[3] = { 0x20, 0x40, 0x80 };	/* CMP0/1/2EN */

static uint32_t tca_top(avr_tca_t *p)
{
	avr_t *avr = p->io.avr;
	return avr->data[p->r_per] | (avr->data[p->r_per + 1] << 8);
}

static uint32_t tca_cmp(avr_tca_t *p, int ch)
{
	avr_t *avr = p->io.avr;
	return avr->data[p->r_cmp[ch]] | (avr->data[p->r_cmp[ch] + 1] << 8);
}

static uint32_t tca_cnt_now(avr_tca_t *p)
{
	avr_t *avr = p->io.avr;
	uint32_t top = tca_top(p);
	if (!p->prescale)
		return 0;
	uint32_t ticks = (uint32_t)((avr->cycle - p->start_cycle) / p->prescale);
	return ticks % (top + 1);
}

static int tca_enabled(avr_tca_t *p)
{
	return (p->io.avr->data[p->r_ctrla] & ENABLE_bm) != 0;
}

/* Smallest event count strictly greater than 'from' (a compare, or TOP+1 wrap). */
static uint32_t tca_next_target(avr_tca_t *p, uint32_t from)
{
	uint32_t top = tca_top(p);
	uint32_t best = top + 1;	/* overflow / wrap */
	for (int ch = 0; ch < 3; ch++) {
		uint32_t c = tca_cmp(p, ch);
		if (c >= 1 && c <= top && c > from && c < best)
			best = c;
	}
	return best;
}

/*
 * Single-slope PWM waveform-output level for channel ch at counter value cnt.
 * DS40002205A 20.3.3.4.3: the output is set at BOTTOM and cleared on the
 * CNT==CMPn compare match, so WOn is high while CNT < CMPn. CMPn == BOTTOM
 * produces a static low; CMPn > TOP a static high. The output is only driven
 * when the channel is enabled (CMPnEN) in a waveform-generation mode; only
 * SINGLESLOPE is modelled (see avr_tca.h), so every other WGMODE reads low.
 */
static uint8_t tca_wo_level(avr_tca_t *p, int ch, uint32_t cnt)
{
	uint8_t ctrlb = p->io.avr->data[p->r_ctrlb];
	uint32_t cmp, top;

	if (!tca_enabled(p) || !(ctrlb & cmpen_bm[ch]))
		return 0;
	if ((ctrlb & WGMODE_gm) != WGMODE_SINGLESLOPE)
		return 0;
	cmp = tca_cmp(p, ch);
	top = tca_top(p);
	if (cmp == 0)
		return 0;
	if (cmp > top)
		return 1;
	return cnt < cmp ? 1 : 0;
}

/* Recompute each WOn level from the live counter and publish any transition. */
static void tca_emit_wo(avr_tca_t *p)
{
	avr_t *avr = p->io.avr;
	uint32_t cnt = tca_enabled(p) ? tca_cnt_now(p)
		: (avr->data[p->r_cnt] | (avr->data[p->r_cnt + 1] << 8));

	for (int ch = 0; ch < 3; ch++) {
		uint8_t lvl = tca_wo_level(p, ch, cnt);
		if (lvl != p->wo_level[ch]) {
			p->wo_level[ch] = lvl;
			avr_raise_irq(p->io.irq + AVR_TCA_IRQ_WO0 + ch, lvl);
		}
	}
}

static avr_cycle_count_t
avr_tca_event(struct avr_t *avr, avr_cycle_count_t when, void *param)
{
	avr_tca_t *p = (avr_tca_t *)param;
	uint32_t top = tca_top(p);
	uint32_t from;

	if (!tca_enabled(p))
		return 0;

	if (p->ev_target > top) {
		/* Overflow / wrap. */
		avr_raise_interrupt(avr, &p->ovf);
		p->start_cycle = when;	/* CNT == 0 again */
		from = 0;
		/* A compare value of 0 matches at CNT == 0 (the wrap point); the
		 * forward-target search only considers values >= 1, so fire it here. */
		for (int ch = 0; ch < 3; ch++)
			if (tca_cmp(p, ch) == 0)
				avr_raise_interrupt(avr, &p->cmp[ch]);
	} else {
		/* Compare match: set every channel whose value equals this target. */
		for (int ch = 0; ch < 3; ch++)
			if (tca_cmp(p, ch) == p->ev_target)
				avr_raise_interrupt(avr, &p->cmp[ch]);
		from = p->ev_target;
	}

	uint32_t target = tca_next_target(p, from);
	p->ev_target = target;
	avr_cycle_count_t next = p->start_cycle +
				(avr_cycle_count_t)target * p->prescale;
	if (next <= when)
		next = when + 1;
	/* Wrap set the output at BOTTOM; a compare match cleared its channel. */
	tca_emit_wo(p);
	return next;
}

static void
avr_tca_reschedule(avr_tca_t *p)
{
	avr_t *avr = p->io.avr;

	avr_cycle_timer_cancel(avr, avr_tca_event, p);
	if (!tca_enabled(p)) {
		tca_emit_wo(p);	/* drives every WOn low */
		return;
	}

	uint8_t clksel = (avr->data[p->r_ctrla] & CLKSEL_gm) >> CLKSEL_gp;
	p->prescale = clksel_div[clksel & 7];

	/* Keep CNT continuity from its current register value. */
	uint32_t cnt = avr->data[p->r_cnt] | (avr->data[p->r_cnt + 1] << 8);
	p->start_cycle = avr->cycle - (avr_cycle_count_t)cnt * p->prescale;

	p->ev_target = tca_next_target(p, cnt);
	avr_cycle_count_t abs = p->start_cycle +
				(avr_cycle_count_t)p->ev_target * p->prescale;
	avr_cycle_count_t rel = (abs > avr->cycle) ? (abs - avr->cycle) : 1;
	avr_cycle_timer_register(avr, rel, avr_tca_event, p);
	tca_emit_wo(p);
}

static void
avr_tca_ctrla_write(struct avr_t *avr, avr_io_addr_t addr,
					uint8_t v, void *param)
{
	avr_tca_t *p = (avr_tca_t *)param;
	avr_core_watch_write(avr, p->r_ctrla, v);
	avr_tca_reschedule(p);
}

static void
avr_tca_ctrlb_write(struct avr_t *avr, avr_io_addr_t addr,
					uint8_t v, void *param)
{
	avr_tca_t *p = (avr_tca_t *)param;
	avr_core_watch_write(avr, p->r_ctrlb, v);
	avr_tca_reschedule(p);
}

/* PER / CMP0-2: changing the period or a compare value moves the next event,
 * so re-evaluate the schedule if the timer is running (cf. avr_tcd_cfg_write). */
static void
avr_tca_cfg_write(struct avr_t *avr, avr_io_addr_t addr,
				  uint8_t v, void *param)
{
	avr_tca_t *p = (avr_tca_t *)param;
	avr_core_watch_write(avr, addr, v);	/* low or high byte */
	if (tca_enabled(p))
		avr_tca_reschedule(p);
}

static uint8_t
avr_tca_cnt_read(struct avr_t *avr, avr_io_addr_t addr, void *param)
{
	avr_tca_t *p = (avr_tca_t *)param;
	uint16_t cnt = tca_enabled(p) ? tca_cnt_now(p) :
				   (avr->data[p->r_cnt] | (avr->data[p->r_cnt + 1] << 8));
	avr->data[p->r_cnt] = cnt & 0xff;
	avr->data[p->r_cnt + 1] = cnt >> 8;	/* latched high byte */
	return cnt & 0xff;
}

static void
avr_tca_cnt_write(struct avr_t *avr, avr_io_addr_t addr,
				  uint8_t v, void *param)
{
	avr_tca_t *p = (avr_tca_t *)param;
	avr_core_watch_write(avr, addr, v);	/* low or high byte */
	if (tca_enabled(p))
		avr_tca_reschedule(p);	/* re-anchor the phase to the new CNT */
}

static void
avr_tca_intflags_write(struct avr_t *avr, avr_io_addr_t addr,
					   uint8_t v, void *param)
{
	avr_tca_t *p = (avr_tca_t *)param;
	uint8_t res = avr->data[p->r_intflags] & ~v;	/* write-1-to-clear */
	avr_core_watch_write(avr, p->r_intflags, res);
	if (!(res & OVF_bm))
		avr_clear_interrupt(avr, &p->ovf);
	for (int ch = 0; ch < 3; ch++)
		if (!(res & cmp_flag[ch]))
			avr_clear_interrupt(avr, &p->cmp[ch]);
}

static void
avr_tca_reset(avr_io_t *io)
{
	avr_tca_t *p = (avr_tca_t *)io;
	avr_cycle_timer_cancel(p->io.avr, avr_tca_event, p);
	p->start_cycle = 0;
	p->prescale = 1;
	p->ev_target = 0;
	for (int ch = 0; ch < 3; ch++)
		p->wo_level[ch] = 0;
}

static const char *irq_names[AVR_TCA_IRQ_COUNT] = {
	[AVR_TCA_IRQ_WO0] = ">tca.wo0",
	[AVR_TCA_IRQ_WO1] = ">tca.wo1",
	[AVR_TCA_IRQ_WO2] = ">tca.wo2",
};

static avr_io_t _io = {
	.kind = "tca",
	.reset = avr_tca_reset,
	.irq_names = irq_names,
};

static void
tca_vector(avr_int_vector_t *v, uint8_t vnum, avr_io_addr_t reg, uint8_t bit)
{
	v->vector = vnum;
	v->enable.reg = reg;	/* INTCTRL bit */
	v->enable.bit = bit;
	v->enable.mask = 1;
	v->raised.reg = reg + 1;	/* INTFLAGS is INTCTRL + 1 */
	v->raised.bit = bit;
	v->raised.mask = 1;
	v->raise_sticky = 1;
}

void
avr_tca_init(
		avr_t * avr,
		avr_tca_t * p,
		avr_io_addr_t base,
		uint8_t vec_ovf,
		uint8_t vec_cmp0,
		uint8_t vec_cmp1,
		uint8_t vec_cmp2,
		char name)
{
	memset(p, 0, sizeof(*p));
	p->io = _io;
	p->name = name;
	p->base = base;
	p->r_ctrla = base + TCAR_CTRLA;
	p->r_ctrlb = base + TCAR_CTRLB;
	p->r_intctrl = base + TCAR_INTCTRL;
	p->r_intflags = base + TCAR_INTFLAGS;
	p->r_cnt = base + TCAR_CNTL;
	p->r_per = base + TCAR_PERL;
	p->r_cmp[0] = base + TCAR_CMP0L;
	p->r_cmp[1] = base + TCAR_CMP1L;
	p->r_cmp[2] = base + TCAR_CMP2L;
	p->prescale = 1;

	/* INTCTRL/INTFLAGS share bit positions: OVF=0, CMP0=4, CMP1=5, CMP2=6. */
	tca_vector(&p->ovf, vec_ovf, p->r_intctrl, 0);
	tca_vector(&p->cmp[0], vec_cmp0, p->r_intctrl, 4);
	tca_vector(&p->cmp[1], vec_cmp1, p->r_intctrl, 5);
	tca_vector(&p->cmp[2], vec_cmp2, p->r_intctrl, 6);

	avr_register_io(avr, &p->io);
	avr_io_setirqs(&p->io, AVR_IOCTL_TCA_GETIRQ(name), AVR_TCA_IRQ_COUNT, NULL);
	p->base_irq = p->io.irq[0].irq;
	avr_register_vector(avr, &p->ovf);
	avr_register_vector(avr, &p->cmp[0]);
	avr_register_vector(avr, &p->cmp[1]);
	avr_register_vector(avr, &p->cmp[2]);

	avr_register_io_write(avr, p->r_ctrla, avr_tca_ctrla_write, p);
	avr_register_io_write(avr, p->r_ctrlb, avr_tca_ctrlb_write, p);
	avr_register_io_write(avr, p->r_intflags, avr_tca_intflags_write, p);
	avr_register_io_read(avr, p->r_cnt, avr_tca_cnt_read, p);
	avr_register_io_write(avr, p->r_cnt, avr_tca_cnt_write, p);
	avr_register_io_write(avr, p->r_cnt + 1, avr_tca_cnt_write, p);

	/* PER and the three compare registers (both bytes) move scheduled events. */
	avr_register_io_write(avr, p->r_per, avr_tca_cfg_write, p);
	avr_register_io_write(avr, p->r_per + 1, avr_tca_cfg_write, p);
	for (int ch = 0; ch < 3; ch++) {
		avr_register_io_write(avr, p->r_cmp[ch], avr_tca_cfg_write, p);
		avr_register_io_write(avr, p->r_cmp[ch] + 1, avr_tca_cfg_write, p);
	}
}
