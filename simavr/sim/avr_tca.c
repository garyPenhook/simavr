/*
	avr_tca.c

	Modern AVR TCA timer, Normal mode. See avr_tca.h.

	Copyright 2024 simavr modern-AVR fork (garyPenhook)

	This file is part of simavr. GNU GPL v3 or later; see COPYING.
 */

#include <stdio.h>
#include "avr_tca.h"

static const uint16_t tca_presc[8] = { 1, 2, 4, 8, 16, 64, 256, 1024 };

static uint16_t reg16(avr_t * avr, avr_tca_t * p, uint8_t off)
{
	return avr->data[p->r_base + off] | (avr->data[p->r_base + off + 1] << 8);
}

// overflow event: fires every period
static avr_cycle_count_t
avr_tca_ovf(avr_t * avr, avr_cycle_count_t when, void * param)
{
	avr_tca_t * p = (avr_tca_t *)param;
	if (!p->period)
		return 0;
	avr->data[p->r_base + AVR_TCA_INTFLAGS] |= AVR_TCA_INT_OVF;
	avr_raise_interrupt(avr, &p->ovf);
	p->base_cycle = when;
	return when + p->period;
}

// one compare channel; reschedules at the same point in each following period
static avr_cycle_count_t
avr_tca_cmp(avr_t * avr, avr_cycle_count_t when, avr_tca_t * p,
			avr_int_vector_t * v, uint8_t flag)
{
	if (!p->period)
		return 0;
	avr->data[p->r_base + AVR_TCA_INTFLAGS] |= flag;
	avr_raise_interrupt(avr, v);
	return when + p->period;
}

static avr_cycle_count_t avr_tca_cmp0(avr_t *a, avr_cycle_count_t w, void *pr)
{ avr_tca_t *p = pr; return avr_tca_cmp(a, w, p, &p->cmp0, AVR_TCA_INT_CMP0); }
static avr_cycle_count_t avr_tca_cmp1(avr_t *a, avr_cycle_count_t w, void *pr)
{ avr_tca_t *p = pr; return avr_tca_cmp(a, w, p, &p->cmp1, AVR_TCA_INT_CMP1); }
static avr_cycle_count_t avr_tca_cmp2(avr_t *a, avr_cycle_count_t w, void *pr)
{ avr_tca_t *p = pr; return avr_tca_cmp(a, w, p, &p->cmp2, AVR_TCA_INT_CMP2); }

static void
avr_tca_stop(avr_t * avr, avr_tca_t * p)
{
	p->period = 0;
	avr_cycle_timer_cancel(avr, avr_tca_ovf, p);
	avr_cycle_timer_cancel(avr, avr_tca_cmp0, p);
	avr_cycle_timer_cancel(avr, avr_tca_cmp1, p);
	avr_cycle_timer_cancel(avr, avr_tca_cmp2, p);
}

static void
avr_tca_start_stop(avr_t * avr, avr_tca_t * p)
{
	uint8_t ctrla = avr->data[p->r_base + AVR_TCA_CTRLA];

	avr_tca_stop(avr, p);
	if (!(ctrla & AVR_TCA_CTRLA_ENABLE))
		return;
	if (avr->data[p->r_base + AVR_TCA_CTRLD] & AVR_TCA_CTRLD_SPLITM) {
		AVR_LOG(avr, LOG_WARNING, "TCA%c: split mode not implemented\n", p->name);
		return;
	}
	uint8_t clksel = (ctrla & AVR_TCA_CTRLA_CLKSEL_gm) >> AVR_TCA_CTRLA_CLKSEL_gp;
	p->prescaler = tca_presc[clksel & 7];

	uint32_t per = reg16(avr, p, AVR_TCA_PER);
	p->period = ((uint32_t)per + 1) * p->prescaler;
	p->base_cycle = avr->cycle;

	avr_cycle_timer_register(avr, p->period, avr_tca_ovf, p);
	// compare matches occur at CNT==CMPx, i.e. CMPx ticks into the period
	uint32_t c0 = reg16(avr, p, AVR_TCA_CMP0);
	uint32_t c1 = reg16(avr, p, AVR_TCA_CMP1);
	uint32_t c2 = reg16(avr, p, AVR_TCA_CMP2);
	if (c0 <= per) avr_cycle_timer_register(avr, c0 * p->prescaler, avr_tca_cmp0, p);
	if (c1 <= per) avr_cycle_timer_register(avr, c1 * p->prescaler, avr_tca_cmp1, p);
	if (c2 <= per) avr_cycle_timer_register(avr, c2 * p->prescaler, avr_tca_cmp2, p);
}

static void
avr_tca_ctrla_write(avr_t * avr, avr_io_addr_t addr, uint8_t v, void * param)
{
	avr->data[addr] = v;
	avr_tca_start_stop(avr, (avr_tca_t *)param);
}

static void
avr_tca_intflags_write(avr_t * avr, avr_io_addr_t addr, uint8_t v, void * param)
{
	avr_tca_t * p = (avr_tca_t *)param;
	avr->data[addr] &= ~v;		// write-1-to-clear
	if (v & AVR_TCA_INT_OVF)  avr_clear_interrupt(avr, &p->ovf);
	if (v & AVR_TCA_INT_CMP0) avr_clear_interrupt(avr, &p->cmp0);
	if (v & AVR_TCA_INT_CMP1) avr_clear_interrupt(avr, &p->cmp1);
	if (v & AVR_TCA_INT_CMP2) avr_clear_interrupt(avr, &p->cmp2);
}

static uint8_t
avr_tca_cnt_read(avr_t * avr, avr_io_addr_t addr, void * param)
{
	avr_tca_t * p = (avr_tca_t *)param;
	uint16_t cnt = 0;
	if (p->period) {
		uint32_t per = reg16(avr, p, AVR_TCA_PER);
		cnt = ((avr->cycle - p->base_cycle) / p->prescaler) % (per + 1);
	}
	if (addr == p->r_base + AVR_TCA_CNT) {
		avr->data[p->r_base + AVR_TCA_CNT + 1] = cnt >> 8;	// latch high byte
		return cnt & 0xff;
	}
	return avr->data[addr];
}

static void
avr_tca_reset(avr_io_t * io)
{
	avr_tca_t * p = (avr_tca_t *)io;
	avr_tca_stop(p->io.avr, p);
	// PER resets to 0xFFFF
	p->io.avr->data[p->r_base + AVR_TCA_PER] = 0xFF;
	p->io.avr->data[p->r_base + AVR_TCA_PER + 1] = 0xFF;
}

void
avr_tca_init(avr_t * avr, avr_tca_t * p)
{
	p->io.kind = "tca";
	p->io.reset = avr_tca_reset;
	avr_register_io(avr, &p->io);
	if (p->ovf.vector)  avr_register_vector(avr, &p->ovf);
	if (p->cmp0.vector) avr_register_vector(avr, &p->cmp0);
	if (p->cmp1.vector) avr_register_vector(avr, &p->cmp1);
	if (p->cmp2.vector) avr_register_vector(avr, &p->cmp2);

	avr_register_io_write(avr, p->r_base + AVR_TCA_CTRLA, avr_tca_ctrla_write, p);
	avr_register_io_write(avr, p->r_base + AVR_TCA_INTFLAGS, avr_tca_intflags_write, p);
	avr_register_io_read(avr, p->r_base + AVR_TCA_CNT, avr_tca_cnt_read, p);
}
