/*
	avr_tcb.c

	"Modern" AVR (AVRxt) 16-bit Timer/Counter type B (TCB). See avr_tcb.h.

	Scope: the Periodic Interrupt mode (CNTMODE = INT), which is the dominant use
	of a TCB as a periodic tick. The counter is advanced by a simavr cycle timer
	scheduled (CCMP+1)*prescale CPU cycles ahead; on each expiry the CAPT flag is
	set (and the interrupt raised if enabled) and the timer reschedules. Other
	count modes and the event/capture inputs are not modelled (the registers
	still store, so configuring firmware does not misbehave).

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
#include "avr_tcb.h"
#include "sim_cycle_timers.h"

/* CTRLA */
#define ENABLE_bm	0x01
#define CLKSEL_gm	0x06
#define CLKSEL_gp	1
#define CLKSEL_CLKDIV1	0
#define CLKSEL_CLKDIV2	1
#define CLKSEL_CLKTCA	2

/* CTRLB */
#define CNTMODE_gm	0x07
#define CNTMODE_INT	0
#define CNTMODE_TIMEOUT	1

/* INTFLAGS / INTCTRL */
#define CAPT_bm		0x01
#define OVF_bm		0x02

/* STATUS */
#define RUN_bm		0x01

static uint32_t tcb_top(avr_tcb_t *p)
{
	avr_t *avr = p->io.avr;
	return avr->data[p->r_ccmp] | (avr->data[p->r_ccmp + 1] << 8);
}

static uint32_t tcb_prescale(avr_tcb_t *p)
{
	avr_t *avr = p->io.avr;
	switch ((avr->data[p->r_ctrla] & CLKSEL_gm) >> CLKSEL_gp) {
	case CLKSEL_CLKDIV2: return 2;
	case CLKSEL_CLKDIV1:
	case CLKSEL_CLKTCA:	/* no TCA prescaler modelled: treat as CLK_PER */
	default:		return 1;
	}
}

static int tcb_mode_periodic(avr_tcb_t *p)
{
	uint8_t mode = p->io.avr->data[p->r_ctrlb] & CNTMODE_gm;
	return mode == CNTMODE_INT || mode == CNTMODE_TIMEOUT;
}

/* Cycle-timer callback: one period elapsed. */
static avr_cycle_count_t
avr_tcb_tick(struct avr_t *avr, avr_cycle_count_t when, void *param)
{
	avr_tcb_t *p = (avr_tcb_t *)param;

	if (!(avr->data[p->r_ctrla] & ENABLE_bm) || !tcb_mode_periodic(p))
		return 0;	/* stopped */

	/* CAPT (periodic). Sets INTFLAGS.CAPT and raises the interrupt if enabled. */
	avr_raise_interrupt(avr, &p->vect);

	/* Restart the period. */
	p->start_cycle = when;
	p->top = tcb_top(p);
	p->prescale = tcb_prescale(p);
	return when + (avr_cycle_count_t)(p->top + 1) * p->prescale;
}

static void
avr_tcb_reschedule(avr_tcb_t *p)
{
	avr_t *avr = p->io.avr;

	avr_cycle_timer_cancel(avr, avr_tcb_tick, p);

	if ((avr->data[p->r_ctrla] & ENABLE_bm) && tcb_mode_periodic(p)) {
		p->start_cycle = avr->cycle;
		p->top = tcb_top(p);
		p->prescale = tcb_prescale(p);
		avr_core_watch_write(avr, p->r_status,
							 avr->data[p->r_status] | RUN_bm);
		avr_cycle_timer_register(avr,
				(avr_cycle_count_t)(p->top + 1) * p->prescale,
				avr_tcb_tick, p);
	} else {
		avr_core_watch_write(avr, p->r_status,
							 avr->data[p->r_status] & ~RUN_bm);
	}
}

static void
avr_tcb_ctrla_write(struct avr_t *avr, avr_io_addr_t addr,
					uint8_t v, void *param)
{
	avr_tcb_t *p = (avr_tcb_t *)param;
	avr_core_watch_write(avr, p->r_ctrla, v);
	avr_tcb_reschedule(p);
}

static void
avr_tcb_ctrlb_write(struct avr_t *avr, avr_io_addr_t addr,
					uint8_t v, void *param)
{
	avr_tcb_t *p = (avr_tcb_t *)param;
	avr_core_watch_write(avr, p->r_ctrlb, v);
	avr_tcb_reschedule(p);
}

/* Reading CNT low computes the live counter and latches the high byte. */
static uint8_t
avr_tcb_cnt_read(struct avr_t *avr, avr_io_addr_t addr, void *param)
{
	avr_tcb_t *p = (avr_tcb_t *)param;
	uint16_t cnt = 0;

	if ((avr->data[p->r_ctrla] & ENABLE_bm) && tcb_mode_periodic(p) &&
		p->prescale) {
		uint32_t ticks = (uint32_t)((avr->cycle - p->start_cycle) / p->prescale);
		cnt = ticks % (p->top + 1);
	}
	avr->data[p->r_cnt] = cnt & 0xff;
	avr->data[p->r_cnt + 1] = cnt >> 8;	/* latched high byte */
	return cnt & 0xff;
}

static void
avr_tcb_intflags_write(struct avr_t *avr, avr_io_addr_t addr,
					   uint8_t v, void *param)
{
	avr_tcb_t *p = (avr_tcb_t *)param;
	/* write-1-to-clear */
	uint8_t res = avr->data[p->r_intflags] & ~v;
	avr_core_watch_write(avr, p->r_intflags, res);
	if (!(res & CAPT_bm))
		avr_clear_interrupt(avr, &p->vect);
}

static void
avr_tcb_reset(avr_io_t *io)
{
	avr_tcb_t *p = (avr_tcb_t *)io;
	avr_cycle_timer_cancel(p->io.avr, avr_tcb_tick, p);
	p->start_cycle = 0;
	p->prescale = 1;
	p->top = 0;
}

static const char *irq_names[1] = { NULL };

static avr_io_t _io = {
	.kind = "tcb",
	.reset = avr_tcb_reset,
	.irq_names = irq_names,
};

void
avr_tcb_init(
		avr_t * avr,
		avr_tcb_t * p,
		avr_io_addr_t base,
		uint8_t vector,
		char name)
{
	memset(p, 0, sizeof(*p));
	p->io = _io;
	p->name = name;
	p->base = base;
	p->r_ctrla = base + TCBR_CTRLA;
	p->r_ctrlb = base + TCBR_CTRLB;
	p->r_intctrl = base + TCBR_INTCTRL;
	p->r_intflags = base + TCBR_INTFLAGS;
	p->r_status = base + TCBR_STATUS;
	p->r_cnt = base + TCBR_CNTL;
	p->r_ccmp = base + TCBR_CCMPL;
	p->prescale = 1;

	/* CAPT interrupt: enabled by INTCTRL.CAPT, flagged in INTFLAGS.CAPT. */
	p->vect.vector = vector;
	p->vect.enable.reg = p->r_intctrl;
	p->vect.enable.bit = 0;	/* CAPT */
	p->vect.enable.mask = 1;
	p->vect.raised.reg = p->r_intflags;
	p->vect.raised.bit = 0;	/* CAPT */
	p->vect.raised.mask = 1;
	p->vect.raise_sticky = 1;	/* modern INTFLAGS are software-cleared */

	avr_register_io(avr, &p->io);
	avr_register_vector(avr, &p->vect);

	avr_register_io_write(avr, p->r_ctrla, avr_tcb_ctrla_write, p);
	avr_register_io_write(avr, p->r_ctrlb, avr_tcb_ctrlb_write, p);
	avr_register_io_write(avr, p->r_intflags, avr_tcb_intflags_write, p);
	avr_register_io_read(avr, p->r_cnt, avr_tcb_cnt_read, p);
}
