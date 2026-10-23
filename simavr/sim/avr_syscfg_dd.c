/*
	avr_syscfg_dd.c

	AVR Dx SYSCFG / SIGROW / FUSE / LOCK. See avr_syscfg_dd.h.

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
#include "avr_syscfg_dd.h"

/* LOCK.KEY value of an unlocked device (device header: LOCK_KEY initial value). */
#define LOCK_KEY_UNLOCKED	0x5cc5c55cUL

/* Factory-programmed fuse defaults (DS40002413B 8.8.2): WDTCFG, BODCFG, OSCCFG,
 * two reserved bytes, SYSCFG0, SYSCFG1, CODESIZE, BOOTSIZE. */
static const uint8_t fuse_defaults[AVR_FUSE_DD_COUNT] = {
	0x00, 0x00, 0x00, 0x00, 0x00, 0xd0, 0x08, 0x00, 0x00,
};

/* Temperature-sensor calibration placeholders (device specific on real parts):
 * slope 4096 and offset 3584 make the ADC result 3584 - T[K] (33.3.3.8). */
#define TEMPSENSE0_CAL	0x1000
#define TEMPSENSE1_CAL	0x0e00

/* Read-only registers: the CPU cannot write SIGROW, FUSE or LOCK. */
static void
syscfg_dd_ro_write(struct avr_t * avr, avr_io_addr_t addr, uint8_t v, void * param)
{
	(void)avr; (void)addr; (void)v; (void)param;
}

/* The FUSE window reads the live avr->fuse[] bytes (the CPU can read, not write). */
static uint8_t
syscfg_dd_fuse_read(struct avr_t * avr, avr_io_addr_t addr, void * param)
{
	avr_syscfg_dd_t * p = (avr_syscfg_dd_t *)param;
	return avr->fuse[addr - p->fuse_base];
}

static void
syscfg_dd_reset(avr_io_t * io)
{
	avr_syscfg_dd_t * p = (avr_syscfg_dd_t *)io;
	avr_t * avr = p->io.avr;

	avr->data[p->syscfg_base + 1] = p->revid;		// SYSCFG.REVID

	for (int i = 0; i < 3; i++)
		avr->data[p->sigrow_base + SIGROWDD_DEVICEID0 + i] = avr->signature[i];
	avr->data[p->sigrow_base + SIGROWDD_TEMPSENSE0] = TEMPSENSE0_CAL & 0xff;
	avr->data[p->sigrow_base + SIGROWDD_TEMPSENSE0 + 1] = TEMPSENSE0_CAL >> 8;
	avr->data[p->sigrow_base + SIGROWDD_TEMPSENSE1] = TEMPSENSE1_CAL & 0xff;
	avr->data[p->sigrow_base + SIGROWDD_TEMPSENSE1 + 1] = TEMPSENSE1_CAL >> 8;
	/* Deterministic non-zero placeholder serial number. */
	for (int i = 0; i < SIGROWDD_SERNUM_LEN; i++)
		avr->data[p->sigrow_base + SIGROWDD_SERNUM0 + i] = 0x10 + i;

	for (int i = 0; i < AVR_FUSE_DD_COUNT; i++)
		avr->data[p->fuse_base + i] = avr->fuse[i];
	for (int i = 0; i < 4; i++)
		avr->data[p->lock_base + i] = (LOCK_KEY_UNLOCKED >> (8 * i)) & 0xff;
}

static const char * irq_names[1] = { NULL };

static avr_io_t _io = {
	.kind = "syscfg_dd",
	.reset = syscfg_dd_reset,
	.irq_names = irq_names,
};

void
avr_syscfg_dd_init(
		avr_t * avr,
		avr_syscfg_dd_t * p,
		avr_io_addr_t syscfg_base,
		avr_io_addr_t sigrow_base,
		avr_io_addr_t fuse_base,
		avr_io_addr_t lock_base,
		uint8_t revid)
{
	memset(p, 0, sizeof(*p));
	p->io = _io;
	p->syscfg_base = syscfg_base;
	p->sigrow_base = sigrow_base;
	p->fuse_base = fuse_base;
	p->lock_base = lock_base;
	p->revid = revid;

	memcpy(avr->fuse, fuse_defaults, sizeof(fuse_defaults));

	avr_register_io(avr, &p->io);

	avr_register_io_write(avr, syscfg_base + 1, syscfg_dd_ro_write, p);		// REVID
	for (int i = 0; i < 0x20; i++)
		avr_register_io_write(avr, sigrow_base + i, syscfg_dd_ro_write, p);
	for (int i = 0; i < AVR_FUSE_DD_COUNT; i++) {
		avr_register_io_read(avr, fuse_base + i, syscfg_dd_fuse_read, p);
		avr_register_io_write(avr, fuse_base + i, syscfg_dd_ro_write, p);
	}
	for (int i = 0; i < 4; i++)
		avr_register_io_write(avr, lock_base + i, syscfg_dd_ro_write, p);
}
