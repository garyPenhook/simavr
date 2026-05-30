/*
	avr_slpctrl.c

	"Modern" AVR (AVRxt) Sleep Controller. See avr_slpctrl.h.

	Tracks CTRLA.SEN into avr->arch.sleep_enabled so the modern SLEEP path only
	sleeps while sleep is enabled; SMODE is stored for read-back.

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
#include "avr_slpctrl.h"

/* CTRLA */
#define SEN_bm	0x01

static void
avr_slpctrl_ctrla_write(struct avr_t *avr, avr_io_addr_t addr, uint8_t v,
						void *param)
{
	(void)param;
	avr_core_watch_write(avr, addr, v);
	avr->arch.sleep_enabled = (v & SEN_bm) ? 1 : 0;
}

static void
avr_slpctrl_reset(avr_io_t *io)
{
	avr_slpctrl_t *p = (avr_slpctrl_t *)io;
	p->io.avr->arch.sleep_enabled = 0;
}

static const char *irq_names[1] = { NULL };

static avr_io_t _io = {
	.kind = "slpctrl",
	.reset = avr_slpctrl_reset,
	.irq_names = irq_names,
};

void
avr_slpctrl_init(
		avr_t * avr,
		avr_slpctrl_t * p,
		avr_io_addr_t base,
		char name)
{
	memset(p, 0, sizeof(*p));
	p->io = _io;
	p->name = name;
	p->base = base;
	p->r_ctrla = base + SLPCTRLR_CTRLA;

	avr_register_io(avr, &p->io);
	avr_register_io_write(avr, p->r_ctrla, avr_slpctrl_ctrla_write, p);
}
