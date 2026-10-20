/*
	avr_regmask.c

	Reserved bits and special write rules for the plain registers of a core;
	see avr_regmask.h.

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

#include <stddef.h>
#include "avr_regmask.h"
#include "sim_cycle_timers.h"

#define BODSE_bm	0x01
#define BODS_bm		0x02
#define BODSE_CYCLES	4	// "within four clock cycles"
#define BODS_CYCLES	3	// "automatically cleared after three clock cycles"

static avr_cycle_count_t
bodse_expire(struct avr_t * avr, avr_cycle_count_t when, void * param)
{
	avr_regmask_entry_t * e = (avr_regmask_entry_t *)param;

	avr->data[e->reg] &= ~BODSE_bm;
	return 0;
}

static avr_cycle_count_t
bods_expire(struct avr_t * avr, avr_cycle_count_t when, void * param)
{
	avr_regmask_entry_t * e = (avr_regmask_entry_t *)param;

	avr->data[e->reg] &= ~BODS_bm;
	return 0;
}

static void
bodcr_write(avr_t * avr, avr_regmask_entry_t * e, uint8_t v)
{
	uint8_t cur = avr->data[e->reg];

	v &= e->mask;
	if (v & BODSE_bm) {
		// opens the window; BODS written together with BODSE is step one of the sequence
		if (!(cur & BODSE_bm))
			avr_cycle_timer_register(avr, BODSE_CYCLES, bodse_expire, e);
		avr->data[e->reg] = v;
		if (v & BODS_bm)		// BODS never outlives three cycles, whether or not the sequence completes
			avr_cycle_timer_register(avr, BODS_CYCLES, bods_expire, e);
	} else if ((cur & BODSE_bm) && (v & BODS_bm)) {
		// step two: BODS = 1, BODSE = 0 inside the window; BODS is active for three cycles
		avr_cycle_timer_cancel(avr, bodse_expire, e);
		avr->data[e->reg] = BODS_bm;
		avr_cycle_timer_register(avr, BODS_CYCLES, bods_expire, e);
	} else {
		// outside the sequence BODS can not be set
		avr->data[e->reg] = 0;
	}
}

static void
regmask_write(avr_t * avr, avr_io_addr_t addr, uint8_t v, void * param)
{
	avr_regmask_entry_t * e = (avr_regmask_entry_t *)param;

	switch (e->kind) {
		case AVR_REGMASK_CLEAR_ONLY:
			avr_core_watch_write(avr, addr, avr->data[addr] & v & e->mask);
			break;
		case AVR_REGMASK_BODCR:
			bodcr_write(avr, e, v);
			break;
		default:
			if (e->shared)
				avr->data[addr] &= e->mask;	// the owning model already stored what it wanted
			else
				avr_core_watch_write(avr, addr, v & e->mask);
			break;
	}
}

static avr_io_t _io = {
	.kind = "regmask",
};

void
avr_regmask_init(avr_t * avr, avr_regmask_t * p)
{
	p->io = _io;
	avr_register_io(avr, &p->io);

	for (avr_regmask_entry_t * e = p->entry; e < p->entry + AVR_REGMASK_MAX && e->reg; e++) {
		avr_io_addr_t a = AVR_DATA_TO_IO(e->reg);

		e->owner = p;
		e->shared = avr->io[a].w.c != NULL;
		avr_register_io_write(avr, e->reg, regmask_write, e);
	}
}
