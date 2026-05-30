/*
	avr_crcscan.c

	"Modern" AVR (AVRxt) CRC memory scan. See avr_crcscan.h.

	Enabling CRCSCAN (CTRLA.ENABLE) reports a successful scan immediately:
	STATUS.OK is set and BUSY is left clear, so `while (CRCSCAN.STATUS & BUSY)`
	polls complete and an OK check passes. Setting NMIEN locks CTRLA until reset
	(the device cannot disable a CRC scan that arms the NMI); the RESET strobe
	clears the result and disables the peripheral.

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
#include "avr_crcscan.h"

/* CTRLA */
#define ENABLE_bm	0x01
#define NMIEN_bm	0x02
#define RESET_bm	0x80

/* STATUS */
#define BUSY_bm		0x01
#define OK_bm		0x02

static void
avr_crcscan_ctrla_write(struct avr_t *avr, avr_io_addr_t addr, uint8_t v,
						void *param)
{
	avr_crcscan_t *p = (avr_crcscan_t *)param;

	/* Once the NMI is armed CTRLA is read-only until reset. */
	if (p->locked)
		return;

	/* RESET strobe: clear the result and disable; the bit self-clears. */
	if (v & RESET_bm) {
		avr_core_watch_write(avr, p->r_ctrla, 0);
		avr_core_watch_write(avr, p->r_status, 0);
		return;
	}

	avr_core_watch_write(avr, addr, v);

	if (v & ENABLE_bm) {
		/* Instant successful scan of the loaded image: OK set, never BUSY. */
		avr_core_watch_write(avr, p->r_status, OK_bm);
		if (v & NMIEN_bm)
			p->locked = 1;
	} else {
		avr_core_watch_write(avr, p->r_status, 0);
	}
}

static void
avr_crcscan_reset(avr_io_t *io)
{
	avr_crcscan_t *p = (avr_crcscan_t *)io;
	p->locked = 0;
	p->io.avr->data[p->r_status] = 0;
}

static const char *irq_names[1] = { NULL };

static avr_io_t _io = {
	.kind = "crcscan",
	.reset = avr_crcscan_reset,
	.irq_names = irq_names,
};

void
avr_crcscan_init(
		avr_t * avr,
		avr_crcscan_t * p,
		avr_io_addr_t base,
		char name)
{
	memset(p, 0, sizeof(*p));
	p->io = _io;
	p->name = name;
	p->base = base;
	p->r_ctrla = base + CRCSCANR_CTRLA;
	p->r_status = base + CRCSCANR_STATUS;

	avr_register_io(avr, &p->io);
	avr_register_io_write(avr, p->r_ctrla, avr_crcscan_ctrla_write, p);
}
