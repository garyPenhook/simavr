/*
	avr_mvio.c

	AVR Dx multi-voltage I/O monitor. See avr_mvio.h.

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
#include "avr_mvio.h"

#define VDDIO2IE_bm		0x01
#define VDDIO2IF_bm		0x01
#define VDDIO2S_bm		0x01
#define MVSYSCFG_gm		0x18
#define MVSYSCFG_gp		3
#define MVSYSCFG_DUAL	1

static int
mvio_dual(avr_mvio_t * p)
{
	return ((p->io.avr->fuse[p->syscfg1_fuse] & MVSYSCFG_gm) >> MVSYSCFG_gp) == MVSYSCFG_DUAL;
}

/* VDDIO2S: single-supply configuration always reads 1 (DS40002413B 19.3.1). */
static uint8_t
mvio_status(avr_mvio_t * p)
{
	return (!mvio_dual(p) || p->vddio2_ok) ? VDDIO2S_bm : 0;
}

static void
mvio_update(avr_mvio_t * p, int signal_change)
{
	avr_t * avr = p->io.avr;
	uint8_t old = avr->data[p->base + MVIOR_STATUS] & VDDIO2S_bm;
	uint8_t now = mvio_status(p);

	avr_core_watch_write(avr, p->base + MVIOR_STATUS, now);
	avr_raise_irq(p->io.irq + AVR_MVIO_IRQ_OK, now);
	if (signal_change && now != old && mvio_dual(p)) {
		avr_core_watch_write(avr, p->base + MVIOR_INTFLAGS,
							 avr->data[p->base + MVIOR_INTFLAGS] | VDDIO2IF_bm);
		avr_raise_interrupt(avr, &p->vect);
	}
}

static void
mvio_irq_input(struct avr_irq_t * irq, uint32_t value, void * param)
{
	avr_mvio_t * p = (avr_mvio_t *)param;
	(void)irq;
	p->vddio2_ok = value & 1;
	mvio_update(p, 1);
}

static void
mvio_intctrl_write(struct avr_t * avr, avr_io_addr_t addr, uint8_t v, void * param)
{
	avr_mvio_t * p = (avr_mvio_t *)param;
	avr_core_watch_write(avr, addr, v & VDDIO2IE_bm);
	if ((v & VDDIO2IE_bm) && (avr->data[p->base + MVIOR_INTFLAGS] & VDDIO2IF_bm))
		avr_raise_interrupt(avr, &p->vect);
}

static void
mvio_intflags_write(struct avr_t * avr, avr_io_addr_t addr, uint8_t v, void * param)
{
	avr_mvio_t * p = (avr_mvio_t *)param;
	uint8_t res = avr->data[addr] & ~(v & VDDIO2IF_bm);
	avr_core_watch_write(avr, addr, res);
	if (!(res & VDDIO2IF_bm))
		avr_clear_interrupt(avr, &p->vect);
}

/* STATUS is read-only. */
static void
mvio_status_write(struct avr_t * avr, avr_io_addr_t addr, uint8_t v, void * param)
{
	(void)avr; (void)addr; (void)v; (void)param;
}

static void
mvio_reset(avr_io_t * io)
{
	avr_mvio_t * p = (avr_mvio_t *)io;

	memset(&p->io.avr->data[p->base], 0, 3);
	mvio_update(p, 0);
}

static const char * irq_names[AVR_MVIO_IRQ_COUNT] = { "mvio.vddio2", ">mvio.ok" };

static avr_io_t _io = {
	.kind = "mvio",
	.reset = mvio_reset,
	.irq_names = irq_names,
};

void
avr_mvio_init(
		avr_t * avr,
		avr_mvio_t * p,
		avr_io_addr_t base,
		uint8_t vector,
		uint8_t syscfg1_fuse_index,
		char name)
{
	memset(p, 0, sizeof(*p));
	p->io = _io;
	p->base = base;
	p->name = name;
	p->syscfg1_fuse = syscfg1_fuse_index;
	p->vddio2_ok = 1;

	p->vect.vector = vector;
	p->vect.enable.reg = base + MVIOR_INTCTRL;		p->vect.enable.mask = VDDIO2IE_bm;
	p->vect.raised.reg = base + MVIOR_INTFLAGS;	p->vect.raised.mask = VDDIO2IF_bm;
	p->vect.raise_sticky = 1;

	avr_register_io(avr, &p->io);
	avr_register_vector(avr, &p->vect);
	avr_io_setirqs(&p->io, AVR_IOCTL_MVIO_GETIRQ(name), AVR_MVIO_IRQ_COUNT, NULL);
	avr_irq_register_notify(p->io.irq + AVR_MVIO_IRQ_VDDIO2, mvio_irq_input, p);

	avr_register_io_write(avr, base + MVIOR_INTCTRL, mvio_intctrl_write, p);
	avr_register_io_write(avr, base + MVIOR_INTFLAGS, mvio_intflags_write, p);
	avr_register_io_write(avr, base + MVIOR_STATUS, mvio_status_write, p);
}
