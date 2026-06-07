/*
	avr_cpuint.c

	"Modern" AVR (AVRxt) CPU Interrupt Controller register block. See
	avr_cpuint.h for the register map and datasheet references.

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
#include "avr_cpuint.h"
#include "sim_interrupts.h"

/* CTRLA */
#define LVL0RR_bm	0x01	/* not CCP-protected */
#define CVT_bm		0x20	/* CCP-protected */
#define IVSEL_bm	0x40	/* CCP-protected */
#define CTRLA_CCP_bm	(CVT_bm | IVSEL_bm)

/*
 * CTRLA: LVL0RR is freely writable; CVT and IVSEL are under Configuration
 * Change Protection and only change while the CCP window is open (datasheet
 * Table 13-3). The non-protected bit always takes effect; the protected bits
 * are preserved unless unlocked.
 */
static void
avr_cpuint_ctrla_write(struct avr_t *avr, avr_io_addr_t addr, uint8_t v,
						void *param)
{
	avr_cpuint_t *p = (avr_cpuint_t *)param;
	uint8_t old = avr->data[p->r_ctrla];
	uint8_t nv;

	if (avr_ccp_io_write_enabled(avr))
		nv = v & (IVSEL_bm | CVT_bm | LVL0RR_bm);
	else
		nv = (old & CTRLA_CCP_bm) | (v & LVL0RR_bm);

	avr_core_watch_write(avr, p->r_ctrla, nv);
	avr_cpuint_set_lvl0rr(avr, nv & LVL0RR_bm);
	avr_cpuint_set_cvt(avr, nv & CVT_bm);
}

/*
 * STATUS is read-only to firmware (Access: R); the execution-level flags are
 * owned by the interrupt engine. Mirror the live engine state on read and
 * discard writes.
 */
static uint8_t
avr_cpuint_status_read(struct avr_t *avr, avr_io_addr_t addr, void *param)
{
	(void)addr; (void)param;
	return avr_cpuint_get_status(avr);
}

static void
avr_cpuint_status_write(struct avr_t *avr, avr_io_addr_t addr, uint8_t v,
						void *param)
{
	(void)avr; (void)addr; (void)v; (void)param;	/* read-only register */
}

/* LVL0PRI: hardware updates this under round robin, so reads return the live
 * engine value rather than the last firmware write. */
static uint8_t
avr_cpuint_lvl0pri_read(struct avr_t *avr, avr_io_addr_t addr, void *param)
{
	(void)addr; (void)param;
	return avr_cpuint_get_lvl0pri(avr);
}

static void
avr_cpuint_lvl0pri_write(struct avr_t *avr, avr_io_addr_t addr, uint8_t v,
						void *param)
{
	avr_cpuint_t *p = (avr_cpuint_t *)param;
	avr_core_watch_write(avr, p->r_lvl0pri, v);
	avr_cpuint_set_lvl0pri(avr, v);
}

/* LVL1VEC: the vector number promoted to priority level 1 (0 = none). */
static void
avr_cpuint_lvl1vec_write(struct avr_t *avr, avr_io_addr_t addr, uint8_t v,
						void *param)
{
	avr_cpuint_t *p = (avr_cpuint_t *)param;
	avr_core_watch_write(avr, p->r_lvl1vec, v);
	avr_cpuint_set_lvl1vec(avr, v);
}

static void
avr_cpuint_reset(avr_io_t *io)
{
	avr_cpuint_t *p = (avr_cpuint_t *)io;
	avr_t *avr = p->io.avr;

	/* All CPUINT registers reset to 0x00; clear the engine mirror to match. */
	avr_cpuint_set_lvl0rr(avr, 0);
	avr_cpuint_set_cvt(avr, 0);
	avr_cpuint_set_lvl0pri(avr, 0);
	avr_cpuint_set_lvl1vec(avr, 0);
}

static const char *irq_names[1] = { NULL };

static avr_io_t _io = {
	.kind = "cpuint",
	.reset = avr_cpuint_reset,
	.irq_names = irq_names,
};

void
avr_cpuint_init(
		avr_t * avr,
		avr_cpuint_t * p,
		avr_io_addr_t base,
		char name)
{
	memset(p, 0, sizeof(*p));
	p->io = _io;
	p->name = name;
	p->base = base;
	p->r_ctrla = base + CPUINTR_CTRLA;
	p->r_status = base + CPUINTR_STATUS;
	p->r_lvl0pri = base + CPUINTR_LVL0PRI;
	p->r_lvl1vec = base + CPUINTR_LVL1VEC;

	avr_register_io(avr, &p->io);
	avr_register_io_write(avr, p->r_ctrla, avr_cpuint_ctrla_write, p);
	avr_register_io_read(avr, p->r_status, avr_cpuint_status_read, p);
	avr_register_io_write(avr, p->r_status, avr_cpuint_status_write, p);
	avr_register_io_read(avr, p->r_lvl0pri, avr_cpuint_lvl0pri_read, p);
	avr_register_io_write(avr, p->r_lvl0pri, avr_cpuint_lvl0pri_write, p);
	avr_register_io_write(avr, p->r_lvl1vec, avr_cpuint_lvl1vec_write, p);
}
