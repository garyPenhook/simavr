/*
	avr_tcb.c

	"Modern" AVR (AVRxt) 16-bit Timer/Counter type B (TCB). See avr_tcb.h.

	Scope: the common modern-AVR TCB modes:
	  - Periodic Interrupt
	  - Time-Out Check
	  - Input Capture on Event
	  - Input Capture Frequency Measurement
	  - Input Capture Pulse-Width Measurement
	  - Input Capture Frequency and Pulse-Width Measurement

	Event-driven modes consume an EVENT_IN IRQ and publish a CAPT_OUT pulse IRQ so
	the capture event can also act as an EVSYS generator.

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
#define CNTMODE_CAPT	2
#define CNTMODE_FRQ		3
#define CNTMODE_PW		4
#define CNTMODE_FRQPW	5

/* EVCTRL */
#define CAPTEI_bm	0x01
#define EDGE_bm		0x10

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

static uint16_t tcb_cnt_reg(avr_tcb_t *p)
{
	avr_t *avr = p->io.avr;
	return avr->data[p->r_cnt] | (avr->data[p->r_cnt + 1] << 8);
}

static void tcb_write_cnt_reg(avr_tcb_t *p, uint16_t v)
{
	avr_t *avr = p->io.avr;
	avr->data[p->r_cnt] = v & 0xff;
	avr->data[p->r_cnt + 1] = v >> 8;
}

static void tcb_write_ccmp_reg(avr_tcb_t *p, uint16_t v)
{
	avr_t *avr = p->io.avr;
	avr->data[p->r_ccmp] = v & 0xff;
	avr->data[p->r_ccmp + 1] = v >> 8;
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

static uint8_t tcb_mode(avr_tcb_t *p)
{
	return p->io.avr->data[p->r_ctrlb] & CNTMODE_gm;
}

static int tcb_mode_capture_read_clears(avr_tcb_t *p)
{
	switch (tcb_mode(p)) {
	case CNTMODE_CAPT:
	case CNTMODE_FRQ:
	case CNTMODE_PW:
	case CNTMODE_FRQPW:
		return 1;
	default:
		return 0;
	}
}

static int tcb_event_edge_matches(avr_tcb_t *p, uint8_t old_level, uint8_t new_level)
{
	uint8_t evctrl = p->io.avr->data[p->r_evctrl];
	if (!(evctrl & EDGE_bm))
		return old_level == 0 && new_level == 1;	/* rising */
	return old_level == 1 && new_level == 0;		/* falling */
}

static uint16_t tcb_current_cnt(avr_tcb_t *p)
{
	avr_t *avr = p->io.avr;
	if (!(avr->data[p->r_status] & RUN_bm) || !p->prescale)
		return p->freeze_count;
	uint32_t ticks = (uint32_t)((avr->cycle - p->start_cycle) / p->prescale);
	return (uint16_t)(p->start_count + ticks);
}

static void tcb_set_running(avr_tcb_t *p, int running, uint16_t count)
{
	avr_t *avr = p->io.avr;
	p->freeze_count = count;
	tcb_write_cnt_reg(p, count);
	if (running) {
		p->start_cycle = avr->cycle;
		p->start_count = count;
		avr_core_watch_write(avr, p->r_status, avr->data[p->r_status] | RUN_bm);
	} else {
		avr_core_watch_write(avr, p->r_status, avr->data[p->r_status] & ~RUN_bm);
	}
}

static void tcb_raise_capt(avr_tcb_t *p)
{
	avr_t *avr = p->io.avr;
	avr_raise_interrupt(avr, &p->vect);
	avr_raise_irq(p->io.irq + AVR_TCB_IRQ_CAPT_OUT, 1);
	avr_raise_irq(p->io.irq + AVR_TCB_IRQ_CAPT_OUT, 0);
}

static void tcb_raise_ovf(avr_tcb_t *p)
{
	avr_t *avr = p->io.avr;
	avr_core_watch_write(avr, p->r_intflags, avr->data[p->r_intflags] | OVF_bm);
}

static avr_cycle_count_t tcb_schedule_delta(avr_tcb_t *p)
{
	uint8_t mode = tcb_mode(p);
	uint16_t cnt = tcb_current_cnt(p);

	switch (mode) {
	case CNTMODE_INT:
		return (avr_cycle_count_t)(p->top + 1) * p->prescale;
	case CNTMODE_TIMEOUT:
		return (avr_cycle_count_t)(p->top + 1 - cnt) * p->prescale;
	case CNTMODE_CAPT:
	case CNTMODE_FRQ:
	case CNTMODE_PW:
	case CNTMODE_FRQPW:
		return (avr_cycle_count_t)(0x10000u - cnt) * p->prescale;
	default:
		return 0;
	}
}

static avr_cycle_count_t
avr_tcb_tick(struct avr_t *avr, avr_cycle_count_t when, void *param)
{
	avr_tcb_t *p = (avr_tcb_t *)param;
	uint8_t mode = tcb_mode(p);

	if (!(avr->data[p->r_ctrla] & ENABLE_bm) || !(avr->data[p->r_status] & RUN_bm))
		return 0;	/* stopped */

	switch (mode) {
	case CNTMODE_INT:
		tcb_raise_capt(p);
		tcb_set_running(p, 1, 0);
		p->top = tcb_top(p);
		p->prescale = tcb_prescale(p);
		return when + (avr_cycle_count_t)(p->top + 1) * p->prescale;
	case CNTMODE_TIMEOUT:
		/* Timeout hit before the stop edge arrived. Freeze and flag CAPT. */
		tcb_set_running(p, 0, (uint16_t)p->top);
		tcb_raise_capt(p);
		return 0;
	case CNTMODE_CAPT:
	case CNTMODE_FRQ:
	case CNTMODE_PW:
	case CNTMODE_FRQPW:
		tcb_raise_ovf(p);
		tcb_set_running(p, 1, 0);
		return when + (avr_cycle_count_t)0x10000u * p->prescale;
	default:
		tcb_set_running(p, 0, tcb_current_cnt(p));
		return 0;
	}
}

static void
avr_tcb_reschedule(avr_tcb_t *p)
{
	avr_t *avr = p->io.avr;
	uint8_t mode = tcb_mode(p);

	avr_cycle_timer_cancel(avr, avr_tcb_tick, p);
	p->prescale = tcb_prescale(p);
	p->top = tcb_top(p);

	if (!(avr->data[p->r_ctrla] & ENABLE_bm)) {
		tcb_set_running(p, 0, tcb_current_cnt(p));
		return;
	}

	switch (mode) {
	case CNTMODE_INT:
	case CNTMODE_CAPT:
	case CNTMODE_FRQ:
		tcb_set_running(p, 1, tcb_cnt_reg(p));
		break;
	default:
		tcb_set_running(p, 0, tcb_cnt_reg(p));
		break;
	}

	if (avr->data[p->r_status] & RUN_bm)
		avr_cycle_timer_register(avr, tcb_schedule_delta(p), avr_tcb_tick, p);
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
	p->pw_armed = 0;
	p->frqpw_stage = 0;
	avr_tcb_reschedule(p);
}

static void
avr_tcb_evctrl_write(struct avr_t *avr, avr_io_addr_t addr,
					 uint8_t v, void *param)
{
	avr_tcb_t *p = (avr_tcb_t *)param;
	avr_core_watch_write(avr, p->r_evctrl, v);
}

/* Reading CNT low computes the live counter and latches the high byte. */
static uint8_t
avr_tcb_cnt_read(struct avr_t *avr, avr_io_addr_t addr, void *param)
{
	avr_tcb_t *p = (avr_tcb_t *)param;
	uint16_t cnt = tcb_current_cnt(p);
	tcb_write_cnt_reg(p, cnt);	/* latched high byte */
	return cnt & 0xff;
}

/*
 * Writing CCMP (TOP) while the timer runs in a TOP-using mode (Periodic
 * Interrupt / Time-Out Check) moves the CAPT deadline. Per the ATtiny3217
 * datasheet (21.3.3.1.1 / .2): CCMP is not double-buffered, the counter is not
 * reset, and if the new TOP is below the current count the counter runs on to
 * MAX (0xFFFF) and only then wraps to BOTTOM before counting up to TOP. In the
 * capture modes TOP is fixed at MAX and CCMP is the hardware capture
 * destination, so a firmware write there only stores the value.
 */
static void
avr_tcb_ccmp_write(struct avr_t *avr, avr_io_addr_t addr,
				   uint8_t v, void *param)
{
	avr_tcb_t *p = (avr_tcb_t *)param;
	uint8_t mode = tcb_mode(p);

	avr_core_watch_write(avr, addr, v);	/* low or high byte */

	if (!(avr->data[p->r_ctrla] & ENABLE_bm) ||
		!(avr->data[p->r_status] & RUN_bm))
		return;
	if (mode != CNTMODE_INT && mode != CNTMODE_TIMEOUT)
		return;

	uint16_t cnt = tcb_current_cnt(p);
	uint32_t top = tcb_top(p);

	p->prescale = tcb_prescale(p);
	p->top = top;
	tcb_set_running(p, 1, cnt);	/* re-anchor the phase, keep CNT */

	/* Ticks until the next TOP match, with the run-to-MAX wrap when TOP < CNT. */
	avr_cycle_count_t delta = (cnt <= top)
		? (avr_cycle_count_t)(top + 1 - cnt)
		: (avr_cycle_count_t)((0x10000u - cnt) + (top + 1));

	avr_cycle_timer_cancel(avr, avr_tcb_tick, p);
	avr_cycle_timer_register(avr, delta * p->prescale, avr_tcb_tick, p);
}

static uint8_t
avr_tcb_ccmp_read(struct avr_t *avr, avr_io_addr_t addr, void *param)
{
	avr_tcb_t *p = (avr_tcb_t *)param;
	uint8_t v = avr->data[addr];
	if (addr == p->r_ccmp && tcb_mode_capture_read_clears(p)) {
		uint8_t flags = avr->data[p->r_intflags] & ~CAPT_bm;
		avr_core_watch_write(avr, p->r_intflags, flags);
		avr_clear_interrupt(avr, &p->vect);
	}
	return v;
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
avr_tcb_event_input(struct avr_irq_t *irq, uint32_t value, void *param)
{
	avr_tcb_t *p = (avr_tcb_t *)param;
	avr_t *avr = p->io.avr;
	uint8_t old_level = p->event_level;
	uint8_t new_level = value & 1;
	uint8_t mode = tcb_mode(p);

	(void)irq;
	p->event_level = new_level;

	if (!(avr->data[p->r_ctrla] & ENABLE_bm) || !(avr->data[p->r_evctrl] & CAPTEI_bm))
		return;
	if (old_level == new_level)
		return;

	switch (mode) {
	case CNTMODE_TIMEOUT:
		if (!tcb_event_edge_matches(p, old_level, new_level))
			return;
		if (avr->data[p->r_status] & RUN_bm) {
			tcb_set_running(p, 0, tcb_current_cnt(p));
			avr_cycle_timer_cancel(avr, avr_tcb_tick, p);
		} else {
			tcb_set_running(p, 1, 0);
			avr_cycle_timer_register(avr, tcb_schedule_delta(p), avr_tcb_tick, p);
		}
		break;

	case CNTMODE_CAPT:
		if (!tcb_event_edge_matches(p, old_level, new_level))
			return;
		tcb_write_ccmp_reg(p, tcb_current_cnt(p));
		tcb_raise_capt(p);
		break;

	case CNTMODE_FRQ:
		if (!tcb_event_edge_matches(p, old_level, new_level))
			return;
		tcb_write_ccmp_reg(p, tcb_current_cnt(p));
		tcb_raise_capt(p);
		tcb_set_running(p, 1, 0);
		avr_cycle_timer_cancel(avr, avr_tcb_tick, p);
		avr_cycle_timer_register(avr, tcb_schedule_delta(p), avr_tcb_tick, p);
		break;

	case CNTMODE_PW:
		if (old_level == 0 && new_level == 1) {
			p->pw_armed = 1;
			tcb_set_running(p, 1, 0);
			avr_cycle_timer_cancel(avr, avr_tcb_tick, p);
			avr_cycle_timer_register(avr, tcb_schedule_delta(p), avr_tcb_tick, p);
		} else if (old_level == 1 && new_level == 0 && p->pw_armed) {
			tcb_write_ccmp_reg(p, tcb_current_cnt(p));
			tcb_raise_capt(p);
			tcb_set_running(p, 0, tcb_current_cnt(p));
			avr_cycle_timer_cancel(avr, avr_tcb_tick, p);
			p->pw_armed = 0;
		}
		break;

	case CNTMODE_FRQPW:
		if (old_level == 0 && new_level == 1) {
			if (p->frqpw_stage == 0) {
				p->frqpw_stage = 1;
				tcb_set_running(p, 1, 0);
				avr_cycle_timer_cancel(avr, avr_tcb_tick, p);
				avr_cycle_timer_register(avr, tcb_schedule_delta(p), avr_tcb_tick, p);
			} else {
				tcb_set_running(p, 0, tcb_current_cnt(p));
				avr_cycle_timer_cancel(avr, avr_tcb_tick, p);
				tcb_raise_capt(p);
				p->frqpw_stage = 0;
			}
		} else if (old_level == 1 && new_level == 0 && p->frqpw_stage == 1) {
			tcb_write_ccmp_reg(p, tcb_current_cnt(p));
			p->frqpw_stage = 2;
		}
		break;

	default:
		break;
	}
}

static void
avr_tcb_reset(avr_io_t *io)
{
	avr_tcb_t *p = (avr_tcb_t *)io;
	avr_cycle_timer_cancel(p->io.avr, avr_tcb_tick, p);
	p->start_cycle = 0;
	p->prescale = 1;
	p->top = 0;
	p->start_count = 0;
	p->freeze_count = 0;
	p->event_level = 0;
	p->pw_armed = 0;
	p->frqpw_stage = 0;
}

static const char *irq_names[AVR_TCB_IRQ_COUNT] = {
	"tcb.event",
	">tcb.capt",
};

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
	p->r_evctrl = base + TCBR_EVCTRL;
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
	avr_io_setirqs(&p->io, AVR_IOCTL_TCB_GETIRQ(name), AVR_TCB_IRQ_COUNT, NULL);
	p->base_irq = p->io.irq[0].irq;
	avr_register_vector(avr, &p->vect);

	avr_register_io_write(avr, p->r_ctrla, avr_tcb_ctrla_write, p);
	avr_register_io_write(avr, p->r_ctrlb, avr_tcb_ctrlb_write, p);
	avr_register_io_write(avr, p->r_evctrl, avr_tcb_evctrl_write, p);
	avr_register_io_write(avr, p->r_intflags, avr_tcb_intflags_write, p);
	avr_register_io_read(avr, p->r_cnt, avr_tcb_cnt_read, p);
	avr_register_io_read(avr, p->r_ccmp, avr_tcb_ccmp_read, p);
	avr_register_io_write(avr, p->r_ccmp, avr_tcb_ccmp_write, p);
	avr_register_io_write(avr, p->r_ccmp + 1, avr_tcb_ccmp_write, p);
	avr_irq_register_notify(p->io.irq + AVR_TCB_IRQ_EVENT_IN, avr_tcb_event_input, p);
}
