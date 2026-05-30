/*
	avr_tcb.c

	Modern AVR TCB timer, Periodic Interrupt mode. See avr_tcb.h.

	Copyright 2024 simavr modern-AVR fork (garyPenhook)

	This file is part of simavr. GNU GPL v3 or later; see COPYING.
 */

#include <stdio.h>
#include "avr_tcb.h"

static uint16_t tcb_ccmp(avr_t * avr, avr_tcb_t * p)
{
	return avr->data[p->r_base + AVR_TCB_CCMP] |
			(avr->data[p->r_base + AVR_TCB_CCMP + 1] << 8);
}

// fires every period while enabled in periodic-interrupt mode
static avr_cycle_count_t
avr_tcb_tick(avr_t * avr, avr_cycle_count_t when, void * param)
{
	avr_tcb_t * p = (avr_tcb_t *)param;

	if (!p->period)
		return 0;	// stopped
	// set the CAPT flag and raise the interrupt (flag is sticky on modern AVR)
	avr->data[p->r_base + AVR_TCB_INTFLAGS] |= AVR_TCB_INT_CAPT;
	avr_raise_interrupt(avr, &p->capt);
	p->base_cycle = when;
	return when + p->period;	// reschedule
}

static void
avr_tcb_start_stop(avr_t * avr, avr_tcb_t * p)
{
	uint8_t ctrla = avr->data[p->r_base + AVR_TCB_CTRLA];
	uint8_t mode  = avr->data[p->r_base + AVR_TCB_CTRLB] & AVR_TCB_CTRLB_CNTMODE_gm;

	avr_cycle_timer_cancel(avr, avr_tcb_tick, p);
	p->period = 0;

	if (!(ctrla & AVR_TCB_CTRLA_ENABLE))
		return;
	if (mode != 0) {	// only periodic-interrupt mode (CNTMODE=0) is modelled
		AVR_LOG(avr, LOG_WARNING,
				"TCB%c: CNTMODE %d not implemented (only periodic interrupt)\n",
				p->name, mode);
		return;
	}
	uint8_t clksel = (ctrla & AVR_TCB_CTRLA_CLKSEL_gm) >> AVR_TCB_CTRLA_CLKSEL_gp;
	p->clkdiv = (clksel == 1) ? 2 : 1;	// DIV1, DIV2, or TCA-clock (treated /1)
	if (clksel == 2)
		AVR_LOG(avr, LOG_WARNING,
				"TCB%c: CLKSEL=TCA approximated as CLK_PER\n", p->name);

	uint32_t ccmp = tcb_ccmp(avr, p);
	p->period = ((uint32_t)ccmp + 1) * p->clkdiv;	// ticks to reach CCMP
	p->base_cycle = avr->cycle;
	avr_cycle_timer_register(avr, p->period, avr_tcb_tick, p);
}

static void
avr_tcb_ctrla_write(avr_t * avr, avr_io_addr_t addr, uint8_t v, void * param)
{
	avr->data[addr] = v;
	avr_tcb_start_stop(avr, (avr_tcb_t *)param);
}

static void
avr_tcb_intflags_write(avr_t * avr, avr_io_addr_t addr, uint8_t v, void * param)
{
	avr_tcb_t * p = (avr_tcb_t *)param;
	// write-1-to-clear
	avr->data[addr] &= ~v;
	if (v & AVR_TCB_INT_CAPT)
		avr_clear_interrupt(avr, &p->capt);
}

static uint8_t
avr_tcb_cnt_read(avr_t * avr, avr_io_addr_t addr, void * param)
{
	avr_tcb_t * p = (avr_tcb_t *)param;
	uint16_t cnt = 0;
	if (p->period) {
		uint16_t ccmp = tcb_ccmp(avr, p);
		cnt = ((avr->cycle - p->base_cycle) / p->clkdiv) % ((uint32_t)ccmp + 1);
	}
	// low byte read returns low; latch high in TEMP (offset CNT region high)
	if (addr == p->r_base + AVR_TCB_CNT) {
		avr->data[p->r_base + AVR_TCB_CNT + 1] = cnt >> 8;
		return cnt & 0xff;
	}
	return avr->data[addr];
}

static void
avr_tcb_reset(avr_io_t * io)
{
	avr_tcb_t * p = (avr_tcb_t *)io;
	p->period = 0;
	avr_cycle_timer_cancel(p->io.avr, avr_tcb_tick, p);
}

void
avr_tcb_init(avr_t * avr, avr_tcb_t * p)
{
	p->io.kind = "tcb";
	p->io.reset = avr_tcb_reset;
	avr_register_io(avr, &p->io);
	if (p->capt.vector)
		avr_register_vector(avr, &p->capt);

	avr_register_io_write(avr, p->r_base + AVR_TCB_CTRLA, avr_tcb_ctrla_write, p);
	avr_register_io_write(avr, p->r_base + AVR_TCB_INTFLAGS, avr_tcb_intflags_write, p);
	avr_register_io_read(avr, p->r_base + AVR_TCB_CNT, avr_tcb_cnt_read, p);
}
