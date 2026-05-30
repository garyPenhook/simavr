/*
	avr_syscfg.c

	"Modern" AVR (AVRxt) device-identity registers: SYSCFG + the signature row.
	See avr_syscfg.h.

	REVID and SIGROW.DEVICEID[2:0] are read-only and (re)loaded at reset:
	DEVICEID from the core's signature[], REVID from the configured revision.
	EXTBRK is a plain store. Writes to the read-only registers are ignored.

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
#include "avr_syscfg.h"

/* Read-only registers (REVID, DEVICEID): ignore firmware writes. */
static void
avr_syscfg_ro_write(struct avr_t *avr, avr_io_addr_t addr, uint8_t v, void *param)
{
	(void)avr; (void)addr; (void)v; (void)param;
}

static void
avr_syscfg_reset(avr_io_t *io)
{
	avr_syscfg_t *p = (avr_syscfg_t *)io;
	avr_t *avr = p->io.avr;

	avr->data[p->syscfg_base + SYSCFGR_REVID] = p->revid;
	avr->data[p->syscfg_base + SYSCFGR_EXTBRK] = 0;

	/* DEVICEID[2:0] is the device signature firmware reads to identify itself. */
	avr->data[p->sigrow_base + SIGROWR_DEVICEID0] = avr->signature[0];
	avr->data[p->sigrow_base + SIGROWR_DEVICEID1] = avr->signature[1];
	avr->data[p->sigrow_base + SIGROWR_DEVICEID2] = avr->signature[2];

	/* Temperature-sensor calibration (read by the ADC temp channel and firmware). */
	avr->data[p->sigrow_base + SIGROWR_TEMPSENSE0] = AVR_SIGROW_TEMPSENSE0_CAL;
	avr->data[p->sigrow_base + SIGROWR_TEMPSENSE1] = AVR_SIGROW_TEMPSENSE1_CAL;
}

static const char *irq_names[1] = { NULL };

static avr_io_t _io = {
	.kind = "syscfg",
	.reset = avr_syscfg_reset,
	.irq_names = irq_names,
};

void
avr_syscfg_init(
		avr_t * avr,
		avr_syscfg_t * p,
		avr_io_addr_t syscfg_base,
		avr_io_addr_t sigrow_base,
		uint8_t revid)
{
	memset(p, 0, sizeof(*p));
	p->io = _io;
	p->syscfg_base = syscfg_base;
	p->sigrow_base = sigrow_base;
	p->revid = revid;

	avr_register_io(avr, &p->io);

	/* REVID and the three DEVICEID bytes are read-only; EXTBRK is a store. */
	avr_register_io_write(avr, syscfg_base + SYSCFGR_REVID, avr_syscfg_ro_write, p);
	avr_register_io_write(avr, sigrow_base + SIGROWR_DEVICEID0, avr_syscfg_ro_write, p);
	avr_register_io_write(avr, sigrow_base + SIGROWR_DEVICEID1, avr_syscfg_ro_write, p);
	avr_register_io_write(avr, sigrow_base + SIGROWR_DEVICEID2, avr_syscfg_ro_write, p);
}
