/*
	avr_port.c

	Modern AVR (AVRxt) I/O port (PORT) implementation. See avr_port.h.

	Copyright 2024 simavr modern-AVR fork (garyPenhook)

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
#include <stdlib.h>
#include "avr_port.h"

/*
 * Recompute the externally-visible pin levels and raise the per-pin IRQs.
 * Output pins (DIR=1) reflect OUT; input pins reflect the externally driven
 * value kept in the IN register (updated by the input IRQ handler below).
 * The IN register is then refreshed so reads of PORTx.IN observe driven pins.
 */
static void
avr_port_update_pins(avr_port_t * p)
{
	avr_t * avr = p->io.avr;
	uint8_t dir = avr->data[p->r_base + AVR_PORT_DIR];
	uint8_t out = avr->data[p->r_base + AVR_PORT_OUT];
	uint8_t in  = avr->data[p->r_base + AVR_PORT_IN];

	// external defaults apply only to pins set as input
	in = (in & ~p->external_mask) | (p->external_value & p->external_mask);

	uint8_t pins = (out & dir) | (in & ~dir);
	avr->data[p->r_base + AVR_PORT_IN] = pins;

	// guard against the per-pin IRQ notify (avr_port_irq_input) recursing back
	// into us while we report output levels on those same IRQs
	p->irqing = 1;
	for (int i = 0; i < 8; i++)
		avr_raise_irq(p->io.irq + i, (pins >> i) & 1);
	avr_raise_irq(p->io.irq + AVR_PORT_IRQ_PIN_ALL, pins);
	p->irqing = 0;
}

// DIR / OUT direct writes
static void
avr_port_dir_write(avr_t * avr, avr_io_addr_t addr, uint8_t v, void * param)
{
	avr_port_t * p = (avr_port_t *)param;
	avr->data[p->r_base + AVR_PORT_DIR] = v;
	avr_port_update_pins(p);
}

static void
avr_port_out_write(avr_t * avr, avr_io_addr_t addr, uint8_t v, void * param)
{
	avr_port_t * p = (avr_port_t *)param;
	avr->data[p->r_base + AVR_PORT_OUT] = v;
	avr_port_update_pins(p);
}

// SET/CLR/TGL convenience registers act on DIR or OUT; they read back the
// underlying DIR/OUT value.
static void
avr_port_rmw_write(avr_t * avr, avr_io_addr_t addr, uint8_t v, void * param)
{
	avr_port_t * p = (avr_port_t *)param;
	uint8_t off = addr - p->r_base;
	avr_io_addr_t target;	// the underlying DIR or OUT register
	int op;					// 0=set, 1=clr, 2=tgl

	switch (off) {
		case AVR_PORT_DIRSET: target = p->r_base + AVR_PORT_DIR; op = 0; break;
		case AVR_PORT_DIRCLR: target = p->r_base + AVR_PORT_DIR; op = 1; break;
		case AVR_PORT_DIRTGL: target = p->r_base + AVR_PORT_DIR; op = 2; break;
		case AVR_PORT_OUTSET: target = p->r_base + AVR_PORT_OUT; op = 0; break;
		case AVR_PORT_OUTCLR: target = p->r_base + AVR_PORT_OUT; op = 1; break;
		case AVR_PORT_OUTTGL: target = p->r_base + AVR_PORT_OUT; op = 2; break;
		default: return;
	}
	uint8_t cur = avr->data[target];
	cur = op == 0 ? (cur | v) : op == 1 ? (cur & ~v) : (cur ^ v);
	avr->data[target] = cur;
	avr->data[addr] = cur;	// the SET/CLR/TGL register reads back DIR/OUT
	avr_port_update_pins(p);
}

// Writing the IN register toggles OUT (modern behaviour).
static void
avr_port_in_write(avr_t * avr, avr_io_addr_t addr, uint8_t v, void * param)
{
	avr_port_t * p = (avr_port_t *)param;
	avr->data[p->r_base + AVR_PORT_OUT] ^= v;
	avr_port_update_pins(p);
}

// INTFLAGS: write-1-to-clear.
static void
avr_port_intflags_write(avr_t * avr, avr_io_addr_t addr, uint8_t v, void * param)
{
	avr_port_t * p = (avr_port_t *)param;
	avr->data[addr] &= ~v;
	if (p->port_vect.vector)
		avr_clear_interrupt_if(avr, &p->port_vect, v);
}

// External input on a pin (from a board/test): set/clear the IN bit for pins
// configured as inputs.
static void
avr_port_irq_input(struct avr_irq_t * irq, uint32_t value, void * param)
{
	avr_port_t * p = (avr_port_t *)param;
	avr_t * avr = p->io.avr;
	uint8_t mask = 1 << irq->irq;
	uint8_t dir = avr->data[p->r_base + AVR_PORT_DIR];

	if (p->irqing)		// this IRQ was raised by our own output report
		return;
	if (dir & mask)		// pin is an output; external drive ignored
		return;
	if (value)
		avr->data[p->r_base + AVR_PORT_IN] |= mask;
	else
		avr->data[p->r_base + AVR_PORT_IN] &= ~mask;
	avr_port_update_pins(p);
}

static void
avr_port_reset(avr_io_t * io)
{
	avr_port_t * p = (avr_port_t *)io;
	// hook the per-pin IRQs back to ourselves so external code can drive inputs
	for (int i = 0; i < 8; i++)
		avr_irq_register_notify(p->io.irq + i, avr_port_irq_input, p);
}

static int
avr_port_ioctl(avr_io_t * io, uint32_t ctl, void * io_param)
{
	avr_port_t * p = (avr_port_t *)io;
	if (ctl == AVR_IOCTL_PORT_GETIRQ(p->name)) {
		*(avr_irq_t **)io_param = p->io.irq;
		return 0;
	}
	return -1;
}

static const char * avr_port_irq_names[AVR_PORT_IRQ_COUNT] = {
	[AVR_PORT_IRQ_PIN0] = "=pin0", [AVR_PORT_IRQ_PIN1] = "=pin1",
	[AVR_PORT_IRQ_PIN2] = "=pin2", [AVR_PORT_IRQ_PIN3] = "=pin3",
	[AVR_PORT_IRQ_PIN4] = "=pin4", [AVR_PORT_IRQ_PIN5] = "=pin5",
	[AVR_PORT_IRQ_PIN6] = "=pin6", [AVR_PORT_IRQ_PIN7] = "=pin7",
	[AVR_PORT_IRQ_PIN_ALL] = "8=all",
};

void
avr_port_init(avr_t * avr, avr_port_t * p)
{
	p->io.kind = "port.modern";
	p->io.reset = avr_port_reset;
	p->io.ioctl = avr_port_ioctl;
	p->io.irq_names = avr_port_irq_names;
	avr_register_io(avr, &p->io);
	avr_io_setirqs(&p->io,
			AVR_IOCTL_PORT_GETIRQ(p->name), AVR_PORT_IRQ_COUNT, NULL);
	for (int i = 0; i < AVR_PORT_IRQ_COUNT; i++)
		p->io.irq[i].flags |= IRQ_FLAG_FILTERED;	// notify only on change

	if (p->port_vect.vector)
		avr_register_vector(avr, &p->port_vect);

	avr_register_io_write(avr, p->r_base + AVR_PORT_DIR, avr_port_dir_write, p);
	avr_register_io_write(avr, p->r_base + AVR_PORT_OUT, avr_port_out_write, p);
	avr_register_io_write(avr, p->r_base + AVR_PORT_DIRSET, avr_port_rmw_write, p);
	avr_register_io_write(avr, p->r_base + AVR_PORT_DIRCLR, avr_port_rmw_write, p);
	avr_register_io_write(avr, p->r_base + AVR_PORT_DIRTGL, avr_port_rmw_write, p);
	avr_register_io_write(avr, p->r_base + AVR_PORT_OUTSET, avr_port_rmw_write, p);
	avr_register_io_write(avr, p->r_base + AVR_PORT_OUTCLR, avr_port_rmw_write, p);
	avr_register_io_write(avr, p->r_base + AVR_PORT_OUTTGL, avr_port_rmw_write, p);
	avr_register_io_write(avr, p->r_base + AVR_PORT_IN, avr_port_in_write, p);
	avr_register_io_write(avr, p->r_base + AVR_PORT_INTFLAGS, avr_port_intflags_write, p);
}
