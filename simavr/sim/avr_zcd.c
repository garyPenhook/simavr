/*
	avr_zcd.c

	AVR Dx zero-cross detector. See avr_zcd.h.

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
#include "avr_zcd.h"

#define ENABLE_bm	0x01
#define INVERT_bm	0x08
#define INTMODE_gm	0x03
#define INTMODE_RISING	1
#define INTMODE_FALLING	2
#define INTMODE_BOTH	3
#define STATE_bm	0x10
#define CROSSIF_bm	0x01

static inline uint8_t
reg(avr_zcd_t * p, int off)
{
	return p->io.avr->data[p->base + off];
}

static inline void
set(avr_zcd_t * p, int off, uint8_t v)
{
	avr_core_watch_write(p->io.avr, p->base + off, v);
}

/* Output: above the reference = 1, reversed by INVERT (35.3.2.2). */
static uint8_t
zcd_state(avr_zcd_t * p)
{
	if (!(reg(p, ZCDR_CTRLA) & ENABLE_bm))
		return 0;
	uint8_t s = p->zcin_mv > AVR_ZCD_ZCPINV_MV;
	return (reg(p, ZCDR_CTRLA) & INVERT_bm) ? s ^ 1 : s;
}

static void
zcd_evaluate(avr_zcd_t * p)
{
	uint8_t state = zcd_state(p);

	set(p, ZCDR_STATUS, (reg(p, ZCDR_STATUS) & ~STATE_bm) | (state ? STATE_bm : 0));
	if (state == p->prev_state)
		return;
	p->prev_state = state;
	avr_raise_irq(p->io.irq + AVR_ZCD_IRQ_OUT, state);

	uint8_t mode = reg(p, ZCDR_INTCTRL) & INTMODE_gm;
	if (mode == INTMODE_BOTH || (mode == INTMODE_RISING && state) ||
			(mode == INTMODE_FALLING && !state)) {
		set(p, ZCDR_STATUS, reg(p, ZCDR_STATUS) | CROSSIF_bm);
		avr_raise_interrupt(p->io.avr, &p->vect);
	}
}

static void
zcd_ctrla_write(struct avr_t * avr, avr_io_addr_t addr, uint8_t v, void * param)
{
	avr_zcd_t * p = (avr_zcd_t *)param;
	avr_core_watch_write(avr, addr, v & 0xc9);	// RUNSTDBY, OUTEN, INVERT, ENABLE
	zcd_evaluate(p);
}

static void
zcd_intctrl_write(struct avr_t * avr, avr_io_addr_t addr, uint8_t v, void * param)
{
	(void)param;
	avr_core_watch_write(avr, addr, v & INTMODE_gm);
}

/* STATE is read-only; CROSSIF is write-1-to-clear. */
static void
zcd_status_write(struct avr_t * avr, avr_io_addr_t addr, uint8_t v, void * param)
{
	avr_zcd_t * p = (avr_zcd_t *)param;
	uint8_t res = avr->data[addr] & ~(v & CROSSIF_bm);
	avr_core_watch_write(avr, addr, res);
	if (!(res & CROSSIF_bm))
		avr_clear_interrupt(avr, &p->vect);
}

static void
zcd_irq_input(struct avr_irq_t * irq, uint32_t value, void * param)
{
	avr_zcd_t * p = (avr_zcd_t *)param;
	(void)irq;
	p->zcin_mv = value;
	zcd_evaluate(p);
}

static void
zcd_reset(avr_io_t * io)
{
	avr_zcd_t * p = (avr_zcd_t *)io;

	memset(&p->io.avr->data[p->base], 0, 4);
	p->prev_state = 0;
}

static const char * irq_names[AVR_ZCD_IRQ_COUNT] = { "zcd.zcin", ">zcd.out" };

static avr_io_t _io = {
	.kind = "zcd",
	.reset = zcd_reset,
	.irq_names = irq_names,
};

void
avr_zcd_init(
		avr_t * avr,
		avr_zcd_t * p,
		avr_io_addr_t base,
		uint8_t vector,
		char name)
{
	memset(p, 0, sizeof(*p));
	p->io = _io;
	p->name = name;
	p->base = base;

	/* The interrupt is an edge request flagged in STATUS.CROSSIF; it is enabled by
	 * a non-NONE INTMODE, so INTCTRL (INTMODE bits) is the enable register. */
	p->vect.vector = vector;
	p->vect.enable.reg = base + ZCDR_INTCTRL;	p->vect.enable.mask = INTMODE_gm;
	p->vect.raised.reg = base + ZCDR_STATUS;	p->vect.raised.mask = CROSSIF_bm;
	p->vect.raise_sticky = 1;

	avr_register_io(avr, &p->io);
	avr_register_vector(avr, &p->vect);
	avr_io_setirqs(&p->io, AVR_IOCTL_ZCD_GETIRQ(name), AVR_ZCD_IRQ_COUNT, NULL);
	avr_irq_register_notify(p->io.irq + AVR_ZCD_IRQ_ZCIN, zcd_irq_input, p);

	avr_register_io_write(avr, base + ZCDR_CTRLA, zcd_ctrla_write, p);
	avr_register_io_write(avr, base + ZCDR_INTCTRL, zcd_intctrl_write, p);
	avr_register_io_write(avr, base + ZCDR_STATUS, zcd_status_write, p);
}
