/*
	avr_ac_dd.c

	AVR Dx analog comparator. See avr_ac_dd.h.

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

#include <string.h>
#include "avr_ac_dd.h"

/* CTRLA */
#define ENABLE_bm		0x01
#define HYSMODE_gm		0x06
#define HYSMODE_gp		1
/* MUXCTRL */
#define MUXNEG_gm		0x07
#define MUXPOS_gm		0x38
#define MUXPOS_gp		3
#define INVERT_bm		0x80
#define MUXPOS_AINP3	3
#define MUXPOS_AINP4	4
#define MUXNEG_AINN2	2
#define MUXNEG_AINN3	3
#define MUXNEG_DACREF	4
/* INTCTRL */
#define INTMODE_gm		0x30
#define INTMODE_gp		4
#define INTMODE_BOTHEDGE 0
#define INTMODE_NEGEDGE	2
#define INTMODE_POSEDGE	3
#define CMP_bm			0x01
/* STATUS */
#define CMPSTATE_bm		0x10
#define CMPIF_bm		0x01

static inline uint8_t
reg(avr_ac_dd_t * p, int off)
{
	return p->io.avr->data[p->base + off];
}

static inline void
set(avr_ac_dd_t * p, int off, uint8_t v)
{
	avr_core_watch_write(p->io.avr, p->base + off, v);
}

static int
ac_enabled(avr_ac_dd_t * p)
{
	return (reg(p, ACDD_CTRLA) & ENABLE_bm) != 0;
}

static uint32_t
ac_pos_mv(avr_ac_dd_t * p)
{
	switch ((reg(p, ACDD_MUXCTRL) & MUXPOS_gm) >> MUXPOS_gp) {
	case MUXPOS_AINP3: return p->pos_mv[0];
	case MUXPOS_AINP4: return p->pos_mv[1];
	}
	return 0;		// reserved selection
}

static uint32_t
ac_neg_mv(avr_ac_dd_t * p)
{
	switch (reg(p, ACDD_MUXCTRL) & MUXNEG_gm) {
	case MUXNEG_AINN2: return p->neg_mv[0];
	case MUXNEG_AINN3: return p->neg_mv[1];
	case MUXNEG_DACREF:
		return p->dacref_mv;
	}
	return 0;		// reserved selection
}

/* Half of the typical VHYST (Table 38-26: 10/25/50 mV) on each side. */
static int32_t
ac_hyst_half_mv(avr_ac_dd_t * p)
{
	static const uint8_t half[4] = { 0, 5, 12, 25 };
	return half[(reg(p, ACDD_CTRLA) & HYSMODE_gm) >> HYSMODE_gp];
}

static uint8_t
ac_compute_raw(avr_ac_dd_t * p)
{
	int32_t vp = ac_pos_mv(p), vn = ac_neg_mv(p), h = ac_hyst_half_mv(p);

	if (!h)
		return vp > vn;
	if (p->prev_raw)
		return (vn - vp > h) ? 0 : 1;
	return (vp - vn > h) ? 1 : 0;
}

static uint8_t
ac_compute_state(avr_ac_dd_t * p)
{
	if (!ac_enabled(p))
		return 0;
	uint8_t s = ac_compute_raw(p);
	return (reg(p, ACDD_MUXCTRL) & INVERT_bm) ? s ^ 1 : s;
}

/* Re-evaluate the comparator; update CMPSTATE, publish OUT, flag edges. */
static void
ac_evaluate(avr_ac_dd_t * p)
{
	// VDACREF = DACREF * VREF / 256 (DS40002413B 32.3.2.2)
	p->dacref_mv = (uint32_t)(((uint64_t)reg(p, ACDD_DACREF) * p->vref_mv) >> 8);
	uint8_t state = ac_compute_state(p);

	p->prev_raw = ac_enabled(p) ? ac_compute_raw(p) : 0;
	set(p, ACDD_STATUS, (reg(p, ACDD_STATUS) & ~CMPSTATE_bm) | (state ? CMPSTATE_bm : 0));

	if (state == p->prev_state)
		return;
	p->prev_state = state;
	avr_raise_irq(p->io.irq + AVR_AC_DD_IRQ_OUT, state);

	if (!ac_enabled(p))
		return;
	uint8_t mode = (reg(p, ACDD_INTCTRL) & INTMODE_gm) >> INTMODE_gp;
	if (mode == INTMODE_BOTHEDGE || (mode == INTMODE_POSEDGE && state) ||
			(mode == INTMODE_NEGEDGE && !state)) {
		set(p, ACDD_STATUS, reg(p, ACDD_STATUS) | CMPIF_bm);
		if (reg(p, ACDD_INTCTRL) & CMP_bm)
			avr_raise_interrupt(p->io.avr, &p->vect);
	}
}

static void
ac_cfg_write(struct avr_t * avr, avr_io_addr_t addr, uint8_t v, void * param)
{
	avr_ac_dd_t * p = (avr_ac_dd_t *)param;
	avr_core_watch_write(avr, addr, v);
	ac_evaluate(p);
}

static void
ac_intctrl_write(struct avr_t * avr, avr_io_addr_t addr, uint8_t v, void * param)
{
	avr_ac_dd_t * p = (avr_ac_dd_t *)param;
	avr_core_watch_write(avr, addr, v & (INTMODE_gm | CMP_bm));
	if ((v & CMP_bm) && (reg(p, ACDD_STATUS) & CMPIF_bm))
		avr_raise_interrupt(avr, &p->vect);
}

/* CMPSTATE is read-only; CMPIF is write-1-to-clear. */
static void
ac_status_write(struct avr_t * avr, avr_io_addr_t addr, uint8_t v, void * param)
{
	avr_ac_dd_t * p = (avr_ac_dd_t *)param;
	uint8_t res = avr->data[addr] & ~(v & CMPIF_bm);
	avr_core_watch_write(avr, addr, res);
	if (!(res & CMPIF_bm))
		avr_clear_interrupt(avr, &p->vect);
}

static void
ac_irq_input(struct avr_irq_t * irq, uint32_t value, void * param)
{
	avr_ac_dd_t * p = (avr_ac_dd_t *)param;
	int idx = irq->irq - p->base_irq;

	if (idx == AVR_AC_DD_IRQ_AINP3 || idx == AVR_AC_DD_IRQ_AINP4)
		p->pos_mv[idx - AVR_AC_DD_IRQ_AINP3] = value & 0xffff;
	else if (idx == AVR_AC_DD_IRQ_AINN2 || idx == AVR_AC_DD_IRQ_AINN3)
		p->neg_mv[idx - AVR_AC_DD_IRQ_AINN2] = value & 0xffff;
	else
		return;
	ac_evaluate(p);
}

void
avr_ac_dd_set_vref(avr_ac_dd_t * p, uint32_t vref_mv)
{
	p->vref_mv = vref_mv;
	ac_evaluate(p);
}

static void
ac_reset(avr_io_t * io)
{
	avr_ac_dd_t * p = (avr_ac_dd_t *)io;

	memset(&p->io.avr->data[p->base], 0, 8);
	p->io.avr->data[p->base + ACDD_DACREF] = 0xff;		// DACREF resets to 0xFF
	p->vref_mv = 1024;		// VREF.ACREF resets to REFSEL = 1V024
	p->dacref_mv = 1020;		// 0xFF * 1024 / 256
	p->prev_state = p->prev_raw = 0;
}

static const char * irq_names[AVR_AC_DD_IRQ_COUNT] = {
	"ac.ainp3", "ac.ainp4", "ac.ainn2", "ac.ainn3", ">ac.out",
};

static avr_io_t _io = {
	.kind = "ac_dd",
	.reset = ac_reset,
	.irq_names = irq_names,
};

void
avr_ac_dd_init(
		avr_t * avr,
		avr_ac_dd_t * p,
		avr_io_addr_t base,
		uint8_t vector,
		char name)
{
	memset(p, 0, sizeof(*p));
	p->io = _io;
	p->name = name;
	p->base = base;
	p->vref_mv = 1024;

	p->vect.vector = vector;
	p->vect.enable.reg = base + ACDD_INTCTRL;	p->vect.enable.mask = CMP_bm;
	p->vect.raised.reg = base + ACDD_STATUS;	p->vect.raised.mask = CMPIF_bm;
	p->vect.raise_sticky = 1;

	avr_register_io(avr, &p->io);
	avr_register_vector(avr, &p->vect);
	avr_io_setirqs(&p->io, AVR_IOCTL_AC_GETIRQ(name), AVR_AC_DD_IRQ_COUNT, NULL);
	p->base_irq = p->io.irq[0].irq;
	for (int i = AVR_AC_DD_IRQ_AINP3; i <= AVR_AC_DD_IRQ_AINN3; i++)
		avr_irq_register_notify(p->io.irq + i, ac_irq_input, p);

	avr_register_io_write(avr, base + ACDD_CTRLA, ac_cfg_write, p);
	avr_register_io_write(avr, base + ACDD_MUXCTRL, ac_cfg_write, p);
	avr_register_io_write(avr, base + ACDD_DACREF, ac_cfg_write, p);
	avr_register_io_write(avr, base + ACDD_INTCTRL, ac_intctrl_write, p);
	avr_register_io_write(avr, base + ACDD_STATUS, ac_status_write, p);
}
