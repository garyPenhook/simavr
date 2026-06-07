/*
	avr_tcd.c

	"Modern" AVR (AVRxt) 12-bit Timer/Counter type D.
	See avr_tcd.h.

	A simavr cycle timer steps between compare boundaries and ramp ends. The
	model covers the four waveform-generation modes:
	- One Ramp
	- Two Ramp
	- Four Ramp
	- Dual Slope

	STATUS reads ENRDY|CMDRDY so the double-buffered enable/sync polling protocol
	passes. The TCD clock source is approximated as CLK_PER, then divided by
	SYNCPRES and CNTPRES.

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
#include "avr_tcd.h"
#include "sim_cycle_timers.h"

/* CTRLA */
#define ENABLE_bm	0x01
#define SYNCPRES_gm	0x06
#define SYNCPRES_gp	1
#define CNTPRES_gm	0x18
#define CNTPRES_gp	3

/* CTRLB */
#define WGMODE_gm	0x03
#define WGMODE_ONERAMP	0
#define WGMODE_TWORAMP	1
#define WGMODE_FOURRAMP	2
#define WGMODE_DUALSLOPE 3

/* FAULTCTRL */
#define CMPAEN_bm	0x10
#define CMPBEN_bm	0x20

/* INTCTRL / INTFLAGS */
#define OVF_bm		0x01

/* STATUS */
#define ENRDY_bm	0x01
#define CMDRDY_bm	0x02

static inline uint8_t rd(avr_t *avr, avr_io_addr_t a) { return avr->data[a]; }

static int tcd_enabled(avr_tcd_t *p)
{
	return (rd(p->io.avr, p->r_ctrla) & ENABLE_bm) != 0;
}

static uint32_t reg12(avr_tcd_t *p, avr_io_addr_t a)
{
	avr_t *avr = p->io.avr;
	return (avr->data[a] | (avr->data[a + 1] << 8)) & 0x0fff;
}

static uint32_t tcd_top(avr_tcd_t *p)
{
	return reg12(p, p->r_cmpbclr);
}

static uint8_t tcd_mode(avr_tcd_t *p)
{
	return rd(p->io.avr, p->r_ctrlb) & WGMODE_gm;
}

static void tcd_publish(avr_tcd_t *p, uint8_t mask, uint8_t *cur, uint8_t next)
{
	if (!(rd(p->io.avr, p->r_faultctrl) & mask))
		return;
	if (*cur != next) {
		*cur = next;
		avr_raise_irq(p->io.irq + (mask == CMPAEN_bm ? AVR_TCD_IRQ_WOA : AVR_TCD_IRQ_WOB), next);
	}
}

/* Publish the waveform-output levels for the current up-ramp state. */
static void tcd_emit_state(avr_tcd_t *p, int32_t count)
{
	uint8_t mode = tcd_mode(p);
	uint8_t woa = 0, wob = 0;

	switch (mode) {
	case WGMODE_ONERAMP: {
		uint32_t aset = reg12(p, p->r_cmpaset), aclr = reg12(p, p->r_cmpaclr);
		uint32_t bset = reg12(p, p->r_cmpbset), bclr = reg12(p, p->r_cmpbclr);
		woa = (count >= (int32_t)aset && count < (int32_t)aclr);
		wob = (count >= (int32_t)bset && count < (int32_t)bclr);
		break;
	}
	case WGMODE_TWORAMP:
		if (p->phase == 0) {
			uint32_t aset = reg12(p, p->r_cmpaset), aclr = reg12(p, p->r_cmpaclr);
			woa = (count >= (int32_t)aset && count < (int32_t)aclr);
		} else {
			uint32_t bset = reg12(p, p->r_cmpbset), bclr = reg12(p, p->r_cmpbclr);
			wob = (count >= (int32_t)bset && count < (int32_t)bclr);
		}
		break;
	case WGMODE_FOURRAMP:
		if (p->phase == 1)
			woa = 1;
		else if (p->phase == 3)
			wob = 1;
		break;
	default:
		break;
	}

	tcd_publish(p, CMPAEN_bm, &p->woa, woa);
	tcd_publish(p, CMPBEN_bm, &p->wob, wob);
}

static void tcd_dualslope_action(avr_tcd_t *p, int32_t count)
{
	uint32_t aset = reg12(p, p->r_cmpaset);
	uint32_t bset = reg12(p, p->r_cmpbset);

	if (p->dir < 0) {
		if ((uint32_t)count == bset)
			tcd_publish(p, CMPBEN_bm, &p->wob, 1);
		if ((uint32_t)count == aset)
			tcd_publish(p, CMPAEN_bm, &p->woa, 0);
	} else {
		if ((uint32_t)count == aset)
			tcd_publish(p, CMPAEN_bm, &p->woa, 1);
		if ((uint32_t)count == bset)
			tcd_publish(p, CMPBEN_bm, &p->wob, 0);
	}
}

static uint32_t tcd_phase_limit(avr_tcd_t *p)
{
	switch (tcd_mode(p)) {
	case WGMODE_ONERAMP:
	case WGMODE_DUALSLOPE:
		return tcd_top(p);
	case WGMODE_TWORAMP:
		return p->phase == 0 ? reg12(p, p->r_cmpaclr) : reg12(p, p->r_cmpbclr);
	case WGMODE_FOURRAMP:
		switch (p->phase) {
		case 0: return reg12(p, p->r_cmpaset);
		case 1: return reg12(p, p->r_cmpaclr);
		case 2: return reg12(p, p->r_cmpbset);
		default: return reg12(p, p->r_cmpbclr);
		}
	default:
		return tcd_top(p);
	}
}

static uint32_t tcd_next_delta(avr_tcd_t *p, int32_t count)
{
	uint32_t next = tcd_phase_limit(p) + 1u - (uint32_t)count;
	uint8_t mode = tcd_mode(p);

	if (mode == WGMODE_DUALSLOPE) {
		uint32_t top = tcd_top(p);
		if (p->dir < 0) {
			uint32_t aset = reg12(p, p->r_cmpaset);
			uint32_t bset = reg12(p, p->r_cmpbset);
			next = (uint32_t)count + 1u;
			if ((uint32_t)count > aset && (uint32_t)(count - (int32_t)aset) < next)
				next = (uint32_t)(count - (int32_t)aset);
			if ((uint32_t)count > bset && (uint32_t)(count - (int32_t)bset) < next)
				next = (uint32_t)(count - (int32_t)bset);
			return next;
		}

		next = top + 1u - (uint32_t)count;
		{
			uint32_t aset = reg12(p, p->r_cmpaset);
			uint32_t bset = reg12(p, p->r_cmpbset);
			if ((uint32_t)count < aset && aset - (uint32_t)count < next)
				next = aset - (uint32_t)count;
			if ((uint32_t)count < bset && bset - (uint32_t)count < next)
				next = bset - (uint32_t)count;
		}
		return next;
	}

	if (mode == WGMODE_ONERAMP) {
		uint32_t b[4] = {
			reg12(p, p->r_cmpaset),
			reg12(p, p->r_cmpaclr),
			reg12(p, p->r_cmpbset),
			reg12(p, p->r_cmpbclr),
		};
		for (int i = 0; i < 4; i++)
			if ((uint32_t)count < b[i] && b[i] - (uint32_t)count < next)
				next = b[i] - (uint32_t)count;
		return next;
	}

	if (mode == WGMODE_TWORAMP) {
		uint32_t cmp = p->phase == 0 ? reg12(p, p->r_cmpaset) : reg12(p, p->r_cmpbset);
		if ((uint32_t)count < cmp && cmp - (uint32_t)count < next)
			next = cmp - (uint32_t)count;
		return next;
	}

	return next;
}

/* CPU cycles per TCD count: SYNCPRES (1/2/4/8) * CNTPRES (1/4/32). */
static uint32_t tcd_prescale(avr_tcd_t *p)
{
	avr_t *avr = p->io.avr;
	uint32_t sync = 1u << ((rd(avr, p->r_ctrla) & SYNCPRES_gm) >> SYNCPRES_gp);
	static const uint8_t cnt[4] = { 1, 4, 32, 32 };
	uint32_t c = cnt[(rd(avr, p->r_ctrla) & CNTPRES_gm) >> CNTPRES_gp];
	return sync * c;
}

static avr_cycle_count_t
avr_tcd_tick(struct avr_t *avr, avr_cycle_count_t when, void *param)
{
	avr_tcd_t *p = (avr_tcd_t *)param;
	uint8_t mode = tcd_mode(p);
	uint32_t top = tcd_top(p);
	int32_t count;

	if (!tcd_enabled(p))
		return 0;

	count = p->prescale ?
		p->start_count + (int32_t)(((when - p->start_cycle) / p->prescale) * p->dir) :
		(mode == WGMODE_DUALSLOPE ? -1 : (int32_t)tcd_phase_limit(p) + 1);

	if (mode == WGMODE_DUALSLOPE) {
		if (p->dir < 0 && count < 0) {
			p->start_cycle = when;
			p->start_count = 0;
			p->dir = 1;
			return when + (avr_cycle_count_t)tcd_next_delta(p, 0) * p->prescale;
		}
		if (p->dir > 0 && (uint32_t)count > top) {
			avr_raise_interrupt(avr, &p->ovf);
			p->start_cycle = when;
			p->start_count = (int32_t)top;
			p->dir = -1;
			p->top = top;
			p->prescale = tcd_prescale(p);
			return when + (avr_cycle_count_t)tcd_next_delta(p, p->start_count) * p->prescale;
		}

		tcd_dualslope_action(p, count);
		p->start_cycle = when;
		p->start_count = count;
		return when + (avr_cycle_count_t)tcd_next_delta(p, count) * p->prescale;
	}

	if ((uint32_t)count > tcd_phase_limit(p)) {
		p->start_cycle = when;
		p->start_count = 0;

		if (mode == WGMODE_TWORAMP && p->phase == 0) {
			p->phase = 1;
			tcd_emit_state(p, 0);
			return when + (avr_cycle_count_t)tcd_next_delta(p, 0) * p->prescale;
		}
		if (mode == WGMODE_FOURRAMP && p->phase < 3) {
			p->phase++;
			tcd_emit_state(p, 0);
			return when + (avr_cycle_count_t)tcd_next_delta(p, 0) * p->prescale;
		}

		avr_raise_interrupt(avr, &p->ovf);
		p->phase = 0;
		p->top = top;
		p->prescale = tcd_prescale(p);
		tcd_emit_state(p, 0);
		return when + (avr_cycle_count_t)tcd_next_delta(p, 0) * p->prescale;
	}

	tcd_emit_state(p, count);
	p->start_cycle = when;
	p->start_count = count;
	return when + (avr_cycle_count_t)tcd_next_delta(p, count) * p->prescale;
}

static void
avr_tcd_reschedule(avr_tcd_t *p)
{
	avr_t *avr = p->io.avr;

	avr_cycle_timer_cancel(avr, avr_tcd_tick, p);
	if (!tcd_enabled(p))
		return;

	p->start_cycle = avr->cycle;
	p->top = tcd_top(p);
	p->prescale = tcd_prescale(p);
	p->phase = 0;
	if (tcd_mode(p) == WGMODE_DUALSLOPE) {
		p->dir = -1;
		p->start_count = (int32_t)p->top;
		tcd_publish(p, CMPAEN_bm, &p->woa, 0);
		tcd_publish(p, CMPBEN_bm, &p->wob, 0);
		avr_cycle_timer_register(avr,
			(avr_cycle_count_t)tcd_next_delta(p, p->start_count) * p->prescale,
			avr_tcd_tick, p);
		return;
	}

	p->dir = 1;
	p->start_count = 0;
	tcd_emit_state(p, 0);		/* outputs at count 0 (start of ramp) */
	avr_cycle_timer_register(avr,
			(avr_cycle_count_t)tcd_next_delta(p, 0) * p->prescale, avr_tcd_tick, p);
}

static void
avr_tcd_ctrla_write(struct avr_t *avr, avr_io_addr_t addr, uint8_t v, void *param)
{
	avr_tcd_t *p = (avr_tcd_t *)param;
	avr_core_watch_write(avr, addr, v);
	avr_tcd_reschedule(p);
}

/* CTRLB / FAULTCTRL / the compare registers: store, then re-evaluate the
 * waveform schedule if running (so duty / mode changes take effect). */
static void
avr_tcd_cfg_write(struct avr_t *avr, avr_io_addr_t addr, uint8_t v, void *param)
{
	avr_tcd_t *p = (avr_tcd_t *)param;
	avr_core_watch_write(avr, addr, v);
	if (tcd_enabled(p))
		avr_tcd_reschedule(p);
}

static void
avr_tcd_intflags_write(struct avr_t *avr, avr_io_addr_t addr, uint8_t v, void *param)
{
	avr_tcd_t *p = (avr_tcd_t *)param;
	uint8_t res = avr->data[addr] & ~v;	/* write-1-to-clear */
	avr_core_watch_write(avr, addr, res);
	if (!(res & OVF_bm))
		avr_clear_interrupt(avr, &p->ovf);
}

/* STATUS always reports the double-buffered logic ready. */
static uint8_t
avr_tcd_status_read(struct avr_t *avr, avr_io_addr_t addr, void *param)
{
	(void)avr; (void)param;
	return ENRDY_bm | CMDRDY_bm;
}

static void
avr_tcd_reset(avr_io_t *io)
{
	avr_tcd_t *p = (avr_tcd_t *)io;
	avr_cycle_timer_cancel(p->io.avr, avr_tcd_tick, p);
	p->start_cycle = 0;
	p->start_count = 0;
	p->prescale = 1;
	p->top = 0;
	p->phase = 0;
	p->dir = 1;
	p->woa = p->wob = 0;
	p->io.avr->data[p->r_status] = ENRDY_bm | CMDRDY_bm;
}

static const char *irq_names[AVR_TCD_IRQ_COUNT] = {
	[AVR_TCD_IRQ_WOA] = ">tcd.woa",
	[AVR_TCD_IRQ_WOB] = ">tcd.wob",
};

static avr_io_t _io = {
	.kind = "tcd",
	.reset = avr_tcd_reset,
	.irq_names = irq_names,
};

void
avr_tcd_init(
		avr_t * avr,
		avr_tcd_t * p,
		avr_io_addr_t base,
		uint8_t vec_ovf,
		char name)
{
	memset(p, 0, sizeof(*p));
	p->io = _io;
	p->name = name;
	p->base = base;
	p->r_ctrla = base + TCDR_CTRLA;
	p->r_ctrlb = base + TCDR_CTRLB;
	p->r_intctrl = base + TCDR_INTCTRL;
	p->r_intflags = base + TCDR_INTFLAGS;
	p->r_status = base + TCDR_STATUS;
	p->r_faultctrl = base + TCDR_FAULTCTRL;
	p->r_cmpaset = base + TCDR_CMPASETL;
	p->r_cmpaclr = base + TCDR_CMPACLRL;
	p->r_cmpbset = base + TCDR_CMPBSETL;
	p->r_cmpbclr = base + TCDR_CMPBCLRL;
	p->prescale = 1;

	/* OVF: enabled by INTCTRL.OVF(0), flagged in INTFLAGS.OVF(0). */
	p->ovf.vector = vec_ovf;
	p->ovf.enable.reg = p->r_intctrl;
	p->ovf.enable.bit = 0;
	p->ovf.enable.mask = 1;
	p->ovf.raised.reg = p->r_intflags;
	p->ovf.raised.bit = 0;
	p->ovf.raised.mask = 1;
	p->ovf.raise_sticky = 1;

	avr_register_io(avr, &p->io);
	avr_register_vector(avr, &p->ovf);

	avr_io_setirqs(&p->io, AVR_IOCTL_TCD_GETIRQ(name), AVR_TCD_IRQ_COUNT, NULL);
	p->base_irq = p->io.irq[0].irq;

	avr_register_io_write(avr, p->r_ctrla, avr_tcd_ctrla_write, p);
	avr_register_io_write(avr, p->r_intflags, avr_tcd_intflags_write, p);
	avr_register_io_read(avr, p->r_status, avr_tcd_status_read, p);

	/* CTRLB (mode), FAULTCTRL (output enables) and the four compare registers
	 * all influence the waveform schedule: reschedule when they change. */
	avr_register_io_write(avr, p->r_ctrlb, avr_tcd_cfg_write, p);
	avr_register_io_write(avr, p->r_faultctrl, avr_tcd_cfg_write, p);
	avr_register_io_write(avr, p->r_cmpaset, avr_tcd_cfg_write, p);
	avr_register_io_write(avr, p->r_cmpaset + 1, avr_tcd_cfg_write, p);
	avr_register_io_write(avr, p->r_cmpaclr, avr_tcd_cfg_write, p);
	avr_register_io_write(avr, p->r_cmpaclr + 1, avr_tcd_cfg_write, p);
	avr_register_io_write(avr, p->r_cmpbset, avr_tcd_cfg_write, p);
	avr_register_io_write(avr, p->r_cmpbset + 1, avr_tcd_cfg_write, p);
	avr_register_io_write(avr, p->r_cmpbclr, avr_tcd_cfg_write, p);
	avr_register_io_write(avr, p->r_cmpbclr + 1, avr_tcd_cfg_write, p);
}
