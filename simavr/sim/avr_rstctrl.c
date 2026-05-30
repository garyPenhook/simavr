/*
	avr_rstctrl.c

	"Modern" AVR (AVRxt) Reset Controller. See avr_rstctrl.h.

	Writing SWRR.SWRE performs a software reset the same safe way as the
	watchdog (swap avr->run to a callback that calls avr_reset(), restored from
	the reset hook). The reset hook records the cause in RSTFR: PORF the very
	first time, SWRF after a software reset. RSTFR is write-1-to-clear.

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
#include <stdlib.h>
#include <string.h>
#include "avr_rstctrl.h"

/* RSTFR */
#define PORF_bm		0x01
#define SWRF_bm		0x10

/* SWRR */
#define SWRE_bm		0x01

/* Run callback installed momentarily to perform the reset safely. */
static void rstctrl_do_reset(avr_t *avr)
{
	avr_reset(avr);
}

void
avr_rstctrl_request_reset(avr_rstctrl_t *p, uint8_t cause_bm)
{
	avr_t *avr = p->io.avr;
	p->pending_cause |= cause_bm;
	if (p->sw_reset_pending)
		return;				/* a reset is already armed */
	p->sw_reset_pending = 1;
	p->saved_run = avr->run;
	avr->run = rstctrl_do_reset;
}

static void
avr_rstctrl_swrr_write(struct avr_t *avr, avr_io_addr_t addr, uint8_t v,
					   void *param)
{
	avr_rstctrl_t *p = (avr_rstctrl_t *)param;
	(void)avr; (void)addr;
	if (v & SWRE_bm)
		avr_rstctrl_request_reset(p, SWRF_bm);
}

static void
avr_rstctrl_rstfr_write(struct avr_t *avr, avr_io_addr_t addr, uint8_t v,
						void *param)
{
	(void)param;
	/* write-1-to-clear */
	avr_core_watch_write(avr, addr, avr->data[addr] & ~v);
}

static void
avr_rstctrl_reset(avr_io_t *io)
{
	avr_rstctrl_t *p = (avr_rstctrl_t *)io;
	avr_t *avr = p->io.avr;
	uint8_t cause = p->pending_cause;

	if (p->sw_reset_pending) {
		p->sw_reset_pending = 0;
		avr->run = p->saved_run;	/* restore normal execution */
	}
	p->pending_cause = 0;

	/* The initial power-up sets PORF. */
	if (!p->powered_on) {
		cause |= PORF_bm;
		p->powered_on = 1;
	}
	/* avr_reset() has already zeroed data[]; record the cause flags. */
	avr->data[p->r_rstfr] |= cause;
}

static const char *irq_names[1] = { NULL };

static avr_io_t _io = {
	.kind = "rstctrl",
	.reset = avr_rstctrl_reset,
	.irq_names = irq_names,
};

void
avr_rstctrl_init(
		avr_t * avr,
		avr_rstctrl_t * p,
		avr_io_addr_t base,
		char name)
{
	memset(p, 0, sizeof(*p));
	p->io = _io;
	p->name = name;
	p->base = base;
	p->r_rstfr = base + RSTCTRLR_RSTFR;
	p->r_swrr = base + RSTCTRLR_SWRR;

	avr_register_io(avr, &p->io);
	avr_register_io_write(avr, p->r_rstfr, avr_rstctrl_rstfr_write, p);
	avr_register_io_write(avr, p->r_swrr, avr_rstctrl_swrr_write, p);
}
