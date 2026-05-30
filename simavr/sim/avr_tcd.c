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

static uint32_t tcd_top(avr_tcd_t *p)
{
	avr_t *avr = p->io.avr;
	return (avr->data[p->r_cmpbclr] | (avr->data[p->r_cmpbclr + 1] << 8)) & 0x0fff;
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

	avr_raise_interrupt(avr, &p->ovf);	/* sets OVF, raises if INTCTRL.OVF */

	p->start_cycle = when;
	p->top = tcd_top(p);
	p->prescale = tcd_prescale(p);
	return when + (avr_cycle_count_t)(p->top + 1) * p->prescale;
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
	avr_cycle_timer_register(avr,
			(avr_cycle_count_t)(p->top + 1) * p->prescale, avr_tcd_tick, p);
}

static void
avr_tcd_ctrla_write(struct avr_t *avr, avr_io_addr_t addr, uint8_t v, void *param)
{
	avr_tcd_t *p = (avr_tcd_t *)param;
	avr_core_watch_write(avr, addr, v);
	avr_tcd_reschedule(p);
}

static void
avr_tcd_cmpbclr_write(struct avr_t *avr, avr_io_addr_t addr, uint8_t v, void *param)
{
	avr_tcd_t *p = (avr_tcd_t *)param;
	avr_core_watch_write(avr, addr, v);	/* low or high byte */
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
	p->io.avr->data[p->r_status] = ENRDY_bm | CMDRDY_bm;
}

static const char *irq_names[1] = { NULL };

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
	p->r_intctrl = base + TCDR_INTCTRL;
	p->r_intflags = base + TCDR_INTFLAGS;
	p->r_status = base + TCDR_STATUS;
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

	avr_register_io_write(avr, p->r_ctrla, avr_tcd_ctrla_write, p);
	avr_register_io_write(avr, p->r_intflags, avr_tcd_intflags_write, p);
	avr_register_io_write(avr, p->r_cmpbclr, avr_tcd_cmpbclr_write, p);
	avr_register_io_write(avr, p->r_cmpbclr + 1, avr_tcd_cmpbclr_write, p);
	avr_register_io_read(avr, p->r_status, avr_tcd_status_read, p);
}
