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

/* Read-only registers (REVID, DEVICEID, FUSE window): ignore firmware writes. */
static void
avr_syscfg_ro_write(struct avr_t *avr, avr_io_addr_t addr, uint8_t v, void *param)
{
	(void)avr; (void)addr; (void)v; (void)param;
}

/*
 * Memory-mapped FUSE read-back (DS40002205A 6.10): the CPU can read the fuses
 * but not program them. Return the live avr->fuse[] byte so reads see the
 * loaded fuses and any NVMCTRL FUSEWRITE update; reserved fuse addresses read 0.
 */
static uint8_t
avr_syscfg_fuse_read(struct avr_t *avr, avr_io_addr_t addr, void *param)
{
	avr_syscfg_t *p = (avr_syscfg_t *)param;
	uint8_t idx = addr - p->fuse_base;
	if (idx >= sizeof(avr->fuse))
		return 0;
	return avr->fuse[idx];
}

/* FUSE.LOCKBIT read-back (DS40002205A 6.10.4.9). 0xC5 = unlocked; simavr does
 * not model the UPDI debug lock, so it just reports the stored value. Sits past
 * the FUSE_t window (offset 0x0A) and beyond avr->fuse[], so it has its own
 * store. */
static uint8_t
avr_syscfg_lockbit_read(struct avr_t *avr, avr_io_addr_t addr, void *param)
{
	avr_syscfg_t *p = (avr_syscfg_t *)param;
	(void)avr; (void)addr;
	return p->lockbit;
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

	/* SERNUM[0..9]: deterministic non-zero placeholder (no canonical value). */
	for (int i = 0; i < SIGROWR_SERNUM_LEN; i++)
		avr->data[p->sigrow_base + SIGROWR_SERNUM0 + i] = AVR_SIGROW_SERNUM_BYTE(i);

	/* Temperature-sensor calibration (read by the ADC temp channel and firmware). */
	avr->data[p->sigrow_base + SIGROWR_TEMPSENSE0] = AVR_SIGROW_TEMPSENSE0_CAL;
	avr->data[p->sigrow_base + SIGROWR_TEMPSENSE1] = AVR_SIGROW_TEMPSENSE1_CAL;
	/* OSCnnERRxV: 0 = no calibration error (a valid value); left at 0. */
	avr->data[p->sigrow_base + SIGROWR_OSC16ERR3V] = 0;
	avr->data[p->sigrow_base + SIGROWR_OSC16ERR5V] = 0;
	avr->data[p->sigrow_base + SIGROWR_OSC20ERR3V] = 0;
	avr->data[p->sigrow_base + SIGROWR_OSC20ERR5V] = 0;

	/* The FUSE window is served live by avr_syscfg_fuse_read from avr->fuse[];
	 * mirror the bytes into data[] too so a debugger / raw data peek matches. */
	for (uint8_t i = 0; i < p->fuse_count && i < sizeof(avr->fuse); i++)
		avr->data[p->fuse_base + i] = avr->fuse[i];
	/* LOCKBIT is served live by avr_syscfg_lockbit_read; mirror for data peeks. */
	avr->data[p->fuse_base + AVR_FUSE_LOCKBIT_OFFSET] = p->lockbit;
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
		avr_io_addr_t fuse_base,
		uint8_t fuse_count,
		uint8_t revid)
{
	memset(p, 0, sizeof(*p));
	p->io = _io;
	p->syscfg_base = syscfg_base;
	p->sigrow_base = sigrow_base;
	p->fuse_base = fuse_base;
	p->fuse_count = fuse_count;
	p->lockbit = AVR_FUSE_LOCKBIT_UNLOCKED;
	p->revid = revid;

	avr_register_io(avr, &p->io);

	/* REVID and the three DEVICEID bytes are read-only; EXTBRK is a store. */
	avr_register_io_write(avr, syscfg_base + SYSCFGR_REVID, avr_syscfg_ro_write, p);
	avr_register_io_write(avr, sigrow_base + SIGROWR_DEVICEID0, avr_syscfg_ro_write, p);
	avr_register_io_write(avr, sigrow_base + SIGROWR_DEVICEID1, avr_syscfg_ro_write, p);
	avr_register_io_write(avr, sigrow_base + SIGROWR_DEVICEID2, avr_syscfg_ro_write, p);

	/* FUSE read-back window: live reads from avr->fuse[], writes ignored. */
	for (uint8_t i = 0; i < fuse_count && i < sizeof(avr->fuse); i++) {
		avr_register_io_read(avr, fuse_base + i, avr_syscfg_fuse_read, p);
		avr_register_io_write(avr, fuse_base + i, avr_syscfg_ro_write, p);
	}
	/* FUSE.LOCKBIT (offset 0x0A, past the FUSE_t window): read-back only. */
	avr_register_io_read(avr, fuse_base + AVR_FUSE_LOCKBIT_OFFSET,
						 avr_syscfg_lockbit_read, p);
	avr_register_io_write(avr, fuse_base + AVR_FUSE_LOCKBIT_OFFSET,
						  avr_syscfg_ro_write, p);
}
