/*
	avr_tcd.c

	"Modern" AVR (AVRxt) 12-bit Timer/Counter type D — periodic overflow.
	See avr_tcd.h.

	A simavr cycle timer is scheduled (CMPBCLR+1)*prescale CPU cycles ahead; on
	each expiry the OVF flag is set (TCD0_OVF raised if enabled) and it reschedules
	— a clean periodic source. STATUS reads ENRDY|CMDRDY so the double-buffered
	enable/sync polling protocol passes. The TCD clock source is approximated as
	CLK_PER, then divided by SYNCPRES and CNTPRES.

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

/* One Ramp mode with at least one output enabled in FAULTCTRL. */
static int tcd_oneramp(avr_tcd_t *p)
{
	avr_t *avr = p->io.avr;
	return (rd(avr, p->r_ctrlb) & WGMODE_gm) == WGMODE_ONERAMP &&
		   (rd(avr, p->r_faultctrl) & (CMPAEN_bm | CMPBEN_bm));
}

/* Publish the One Ramp waveform-output levels for the given count, raising the
 * WOA/WOB IRQ on a change. Only the FAULTCTRL-enabled outputs are driven. */
static void tcd_emit(avr_tcd_t *p, uint32_t count)
{
	avr_t *avr = p->io.avr;
	uint8_t fc = rd(avr, p->r_faultctrl);

	if (fc & CMPAEN_bm) {
		uint32_t set = reg12(p, p->r_cmpaset), clr = reg12(p, p->r_cmpaclr);
		uint8_t woa = (count >= set && count < clr);
		if (woa != p->woa) {
			p->woa = woa;
			avr_raise_irq(p->io.irq + AVR_TCD_IRQ_WOA, woa);
		}
	}
	if (fc & CMPBEN_bm) {
		uint32_t set = reg12(p, p->r_cmpbset), clr = reg12(p, p->r_cmpbclr);
		uint8_t wob = (count >= set && count < clr);
		if (wob != p->wob) {
			p->wob = wob;
			avr_raise_irq(p->io.irq + AVR_TCD_IRQ_WOB, wob);
		}
	}
}

/* The next count (> count) at which something happens: an output toggles, or the
 * cycle wraps. The terminal boundary is top+1 (the TCD cycle is top+1 counts);
 * CMPACLR/CMPBCLR(=top) are where the outputs clear. */
static uint32_t tcd_next_boundary(avr_tcd_t *p, uint32_t count, uint32_t top)
{
	uint32_t next = top + 1;	/* wrap / OVF */
	if (tcd_oneramp(p)) {
		uint8_t fc = rd(p->io.avr, p->r_faultctrl);
		uint32_t b[4]; int n = 0;
		if (fc & CMPAEN_bm) {
			b[n++] = reg12(p, p->r_cmpaset);
			b[n++] = reg12(p, p->r_cmpaclr);
		}
		if (fc & CMPBEN_bm) {
			b[n++] = reg12(p, p->r_cmpbset);
			b[n++] = reg12(p, p->r_cmpbclr);	/* = top: WOB clears here */
		}
		for (int i = 0; i < n; i++)
			if (b[i] > count && b[i] < next)
				next = b[i];
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

	if (!tcd_enabled(p))
		return 0;

	/* Which count this event lands on (events are scheduled on boundaries). */
	uint32_t count = p->prescale ?
			(uint32_t)((when - p->start_cycle) / p->prescale) : p->top + 1;
	uint32_t next;

	if (count > p->top) {
		/* End of the TCD cycle: overflow and restart from 0. */
		avr_raise_interrupt(avr, &p->ovf);	/* sets OVF, raises if enabled */
		p->start_cycle = when;
		p->top = tcd_top(p);
		p->prescale = tcd_prescale(p);
		tcd_emit(p, 0);
		next = tcd_next_boundary(p, 0, p->top);
		return when + (avr_cycle_count_t)next * p->prescale;
	}

	/* A mid-ramp compare boundary: update the outputs, schedule the next one. */
	tcd_emit(p, count);
	next = tcd_next_boundary(p, count, p->top);
	return when + (avr_cycle_count_t)(next - count) * p->prescale;
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
	tcd_emit(p, 0);		/* outputs at count 0 (start of ramp) */
	uint32_t next = tcd_next_boundary(p, 0, p->top);
	avr_cycle_timer_register(avr,
			(avr_cycle_count_t)next * p->prescale, avr_tcd_tick, p);
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
	p->prescale = 1;
	p->top = 0;
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
