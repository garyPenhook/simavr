/*
	avr_portmux.c

	"Modern" AVR (AVRxt) Port Multiplexer — configuration store. See
	avr_portmux.h.

	The four CTRL registers store what firmware writes and reset to 0. Pin
	re-routing has no behavioural effect in simavr's wire-IRQ peripheral model,
	so the write handler simply stores; it is kept as the single hook where real
	routing could be added later.

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

#include <string.h>
#include "avr_portmux.h"

static void
avr_portmux_write(struct avr_t *avr, avr_io_addr_t addr, uint8_t v, void *param)
{
	/* Store the routing selection; no behavioural effect in this model. */
	(void)param;
	avr_core_watch_write(avr, addr, v);
}

static void
avr_portmux_reset(avr_io_t *io)
{
	avr_portmux_t *p = (avr_portmux_t *)io;
	avr_t *avr = p->io.avr;
	for (int i = 0; i < AVR_PORTMUX_REGS; i++)
		avr->data[p->base + i] = 0;	/* default routing */
}

static const char *irq_names[1] = { NULL };

static avr_io_t _io = {
	.kind = "portmux",
	.reset = avr_portmux_reset,
	.irq_names = irq_names,
};

void
avr_portmux_init(
		avr_t * avr,
		avr_portmux_t * p,
		avr_io_addr_t base,
		char name)
{
	memset(p, 0, sizeof(*p));
	p->io = _io;
	p->name = name;
	p->base = base;

	avr_register_io(avr, &p->io);
	for (int i = 0; i < AVR_PORTMUX_REGS; i++)
		avr_register_io_write(avr, base + i, avr_portmux_write, p);
}
