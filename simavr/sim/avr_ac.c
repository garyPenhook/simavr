/*
	avr_ac.c

	"Modern" AVR (AVRxt) Analog Comparator. See avr_ac.h.

	The comparator output is recomputed whenever an input voltage or the
	configuration changes: STATE = (V+ > V-) ^ INVERT. The positive input is one
	of AINP0..3 (MUXPOS); the negative input is AINN0/AINN1, the internal VREF or
	the DAC output (MUXNEG). On the CTRLA.INTMODE edge the STATUS.CMP flag is set
	and ACn_AC raised (if INTCTRL.CMP); STATUS.STATE always tracks the live
	output, and the output is mirrored on the OUT IRQ.

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
#include "avr_ac.h"

/* CTRLA */
#define ENABLE_bm	0x01
#define INTMODE_gm	0x30
#define INTMODE_gp	4
#define INTMODE_BOTHEDGE	0
#define INTMODE_NEGEDGE		2
#define INTMODE_POSEDGE		3

/* MUXCTRLA */
#define MUXNEG_gm	0x03
#define MUXPOS_gm	0x18
#define MUXPOS_gp	3
#define INVERT_bm	0x80

/* INTCTRL / STATUS */
#define CMP_bm		0x01
#define STATE_bm	0x10

static inline uint8_t rd(avr_t *avr, avr_io_addr_t a) { return avr->data[a]; }

static int ac_enabled(avr_ac_t *p)
{
	return (rd(p->io.avr, p->r_ctrla) & ENABLE_bm) != 0;
}

/* Positive / negative input voltages (mV) for the current MUX selection. */
static uint32_t ac_pos_mv(avr_ac_t *p)
{
	uint8_t sel = (rd(p->io.avr, p->r_muxctrla) & MUXPOS_gm) >> MUXPOS_gp;
	return p->pos_mv[sel & (AVR_AC_POS_PINS - 1)];
}
static uint32_t ac_neg_mv(avr_ac_t *p)
{
	uint8_t sel = rd(p->io.avr, p->r_muxctrla) & MUXNEG_gm;
	switch (sel) {
	case 0: return p->neg_mv[0];	/* AINN0 */
	case 1: return p->neg_mv[1];	/* AINN1 */
	case 2: return p->vref_mv;	/* internal VREF */
	case 3: return p->dacref_mv;	/* DAC output */
	}
	return 0;
}

/* Combinational comparator output (0/1), honouring INVERT. */
static uint8_t ac_compute_state(avr_ac_t *p)
{
	if (!ac_enabled(p))
		return 0;
	uint8_t s = ac_pos_mv(p) > ac_neg_mv(p);
	if (rd(p->io.avr, p->r_muxctrla) & INVERT_bm)
		s ^= 1;
	return s;
}

/* Re-evaluate the comparator; update STATE, mirror OUT, flag edges. */
static void ac_evaluate(avr_ac_t *p)
{
	avr_t *avr = p->io.avr;
	uint8_t state = ac_compute_state(p);

	/* STATUS.STATE mirrors the live output (preserve the W1C CMP flag). */
	uint8_t status = rd(avr, p->r_status) & ~STATE_bm;
	if (state)
		status |= STATE_bm;
	avr_core_watch_write(avr, p->r_status, status);

	if (state != p->prev_state) {
		avr_raise_irq(p->io.irq + AVR_AC_IRQ_OUT, state);

		if (ac_enabled(p)) {
			uint8_t mode = (rd(avr, p->r_ctrla) & INTMODE_gm) >> INTMODE_gp;
			int edge = (mode == INTMODE_BOTHEDGE) ||
					   (mode == INTMODE_POSEDGE && state) ||
					   (mode == INTMODE_NEGEDGE && !state);
			if (edge) {
				avr_core_watch_write(avr, p->r_status,
									 rd(avr, p->r_status) | CMP_bm);
				if (rd(avr, p->r_intctrl) & CMP_bm)
					avr_raise_interrupt(avr, &p->vect);
			}
		}
		p->prev_state = state;
	}
}

static void
avr_ac_ctrla_write(struct avr_t *avr, avr_io_addr_t addr, uint8_t v, void *param)
{
	avr_ac_t *p = (avr_ac_t *)param;
	avr_core_watch_write(avr, addr, v);
	ac_evaluate(p);
}

static void
avr_ac_muxctrla_write(struct avr_t *avr, avr_io_addr_t addr, uint8_t v,
					  void *param)
{
	avr_ac_t *p = (avr_ac_t *)param;
	avr_core_watch_write(avr, addr, v);
	ac_evaluate(p);
}

static void
avr_ac_intctrl_write(struct avr_t *avr, avr_io_addr_t addr, uint8_t v,
					 void *param)
{
	avr_ac_t *p = (avr_ac_t *)param;
	avr_core_watch_write(avr, addr, v);
	/* Enabling CMP while its flag is already set raises immediately. */
	if ((v & CMP_bm) && (rd(avr, p->r_status) & CMP_bm))
		avr_raise_interrupt(avr, &p->vect);
}

static uint8_t
avr_ac_status_read(struct avr_t *avr, avr_io_addr_t addr, void *param)
{
	avr_ac_t *p = (avr_ac_t *)param;
	/* Refresh the live STATE bit on read (CMP flag untouched). */
	uint8_t status = avr->data[addr] & ~STATE_bm;
	if (ac_compute_state(p))
		status |= STATE_bm;
	avr->data[addr] = status;
	return status;
}

static void
avr_ac_status_write(struct avr_t *avr, avr_io_addr_t addr, uint8_t v,
					void *param)
{
	avr_ac_t *p = (avr_ac_t *)param;
	/* CMP is write-1-to-clear; STATE is read-only. */
	uint8_t res = avr->data[addr] & ~(v & CMP_bm);
	avr_core_watch_write(avr, addr, res);
	if (!(res & CMP_bm))
		avr_clear_interrupt(avr, &p->vect);
}

/* A board/test presents an analog voltage (mV) on an input pin. */
static void
avr_ac_irq_input(struct avr_irq_t *irq, uint32_t value, void *param)
{
	avr_ac_t *p = (avr_ac_t *)param;
	int idx = irq->irq - p->base_irq;

	if (idx >= AVR_AC_IRQ_AINP0 && idx <= AVR_AC_IRQ_AINP3)
		p->pos_mv[idx - AVR_AC_IRQ_AINP0] = value & 0xffff;
	else if (idx == AVR_AC_IRQ_AINN0 || idx == AVR_AC_IRQ_AINN1)
		p->neg_mv[idx - AVR_AC_IRQ_AINN0] = value & 0xffff;
	else
		return;
	ac_evaluate(p);
}

static void
avr_ac_reset(avr_io_t *io)
{
	avr_ac_t *p = (avr_ac_t *)io;
	p->prev_state = 0;
}

static const char *irq_names[AVR_AC_IRQ_COUNT] = {
	[AVR_AC_IRQ_AINP0] = "ac.ainp0",
	[AVR_AC_IRQ_AINP1] = "ac.ainp1",
	[AVR_AC_IRQ_AINP2] = "ac.ainp2",
	[AVR_AC_IRQ_AINP3] = "ac.ainp3",
	[AVR_AC_IRQ_AINN0] = "ac.ainn0",
	[AVR_AC_IRQ_AINN1] = "ac.ainn1",
	[AVR_AC_IRQ_OUT] = ">ac.out",
};

static avr_io_t _io = {
	.kind = "ac",
	.reset = avr_ac_reset,
	.irq_names = irq_names,
};

void
avr_ac_set_refs(avr_ac_t * p, uint32_t vref_mv, uint32_t dacref_mv)
{
	p->vref_mv = vref_mv;
	p->dacref_mv = dacref_mv;
}

void
avr_ac_init(
		avr_t * avr,
		avr_ac_t * p,
		avr_io_addr_t base,
		uint8_t vector,
		char name)
{
	memset(p, 0, sizeof(*p));
	p->io = _io;
	p->name = name;
	p->base = base;
	p->r_ctrla = base + ACR_CTRLA;
	p->r_muxctrla = base + ACR_MUXCTRLA;
	p->r_intctrl = base + ACR_INTCTRL;
	p->r_status = base + ACR_STATUS;
	p->vref_mv = 1100;	/* internal reference default (settable) */
	p->dacref_mv = 0;

	/* ACn_AC: enabled by INTCTRL.CMP(0), flagged in STATUS.CMP(0). */
	p->vect.vector = vector;
	p->vect.enable.reg = p->r_intctrl;
	p->vect.enable.bit = 0;
	p->vect.enable.mask = 1;
	p->vect.raised.reg = p->r_status;
	p->vect.raised.bit = 0;	/* CMP */
	p->vect.raised.mask = 1;
	p->vect.raise_sticky = 1;

	avr_register_io(avr, &p->io);
	avr_register_vector(avr, &p->vect);

	avr_io_setirqs(&p->io, AVR_IOCTL_AC_GETIRQ(name), AVR_AC_IRQ_COUNT, NULL);
	p->base_irq = p->io.irq[0].irq;
	for (int i = AVR_AC_IRQ_AINP0; i <= AVR_AC_IRQ_AINN1; i++)
		avr_irq_register_notify(p->io.irq + i, avr_ac_irq_input, p);

	avr_register_io_write(avr, p->r_ctrla, avr_ac_ctrla_write, p);
	avr_register_io_write(avr, p->r_muxctrla, avr_ac_muxctrla_write, p);
	avr_register_io_write(avr, p->r_intctrl, avr_ac_intctrl_write, p);
	avr_register_io_write(avr, p->r_status, avr_ac_status_write, p);
	avr_register_io_read(avr, p->r_status, avr_ac_status_read, p);
}
