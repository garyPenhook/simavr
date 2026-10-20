/*
	avr_clkpr.c

	Classic AVR system clock prescaler (CLKPR); see avr_clkpr.h.

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
#include "avr_clkpr.h"
#include "sim_cycle_timers.h"

#define CLKPCE_bm	0x80
#define CLKPS_gm	0x0F
#define CLKPCE_CYCLES	4	// "cleared by hardware four cycles after it is written"

/* Frequency someone else (firmware load, --freq) set since we last did: take it as the clock at
 * the division now in force, so the undivided base stays exact across any number of changes. */
static void
clkpr_sync(avr_clkpr_t * p)
{
	if (p->io.avr->frequency != p->last_hz) {
		p->base_hz = (uint64_t)p->io.avr->frequency * p->div;
		p->last_hz = p->io.avr->frequency;
	}
}

static void
clkpr_set_div(avr_clkpr_t * p, uint16_t div)
{
	avr_t * avr = p->io.avr;

	clkpr_sync(p);
	p->div = div;
	avr->frequency = p->last_hz = (uint32_t)((p->base_hz + div / 2) / div);
}

static avr_cycle_count_t
clkpr_expire(struct avr_t * avr, avr_cycle_count_t when, void * param)
{
	avr_clkpr_t * p = (avr_clkpr_t *)param;

	avr->data[p->r_clkpr] &= ~CLKPCE_bm;
	return 0;
}

static void
clkpr_write(struct avr_t * avr, avr_io_addr_t addr, uint8_t v, void * param)
{
	avr_clkpr_t * p = (avr_clkpr_t *)param;
	uint8_t cur = avr->data[addr];

	if ((v & CLKPCE_bm) && !(v & CLKPS_gm)) {
		// CLKPCE is only updated when the other bits are written as zero, and
		// rewriting it neither extends nor clears the time-out
		if (!(cur & CLKPCE_bm)) {
			avr_core_watch_write(avr, addr, cur | CLKPCE_bm);
			avr_cycle_timer_register(avr, CLKPCE_CYCLES, clkpr_expire, p);
		}
		return;
	}
	if ((cur & CLKPCE_bm) && !(v & CLKPCE_bm)) {
		uint8_t n = v & CLKPS_gm;

		avr_cycle_timer_cancel(avr, clkpr_expire, p);
		avr_core_watch_write(avr, addr, n);		// also clears CLKPCE
		if (n <= 8)					// 9..15 are reserved: value kept, clock unchanged
			clkpr_set_div(p, 1 << n);
	}
	// anything else is outside the change window and ignored
}

static void
clkpr_reset(avr_io_t * io)
{
	avr_clkpr_t * p = (avr_clkpr_t *)io;
	avr_t * avr = io->avr;
	// CKDIV8 programmed (bit = 0): CLKPS resets to 0011 (divide by 8), else 0000
	uint8_t n = (avr->fuse[p->ckdiv8_fuse_byte] >> p->ckdiv8_fuse_bit) & 1 ? 0 : 3;

	avr_cycle_timer_cancel(avr, clkpr_expire, p);
	if (p->div != p->reset_div)		// the firmware changed the clock: back to the start-up one
		clkpr_set_div(p, p->reset_div);
	clkpr_sync(p);
	p->reset_div = p->div = 1 << n;
	p->base_hz = (uint64_t)avr->frequency * p->div;
	avr->data[p->r_clkpr] = n;
}

static avr_io_t _io = {
	.kind = "clkpr",
	.reset = clkpr_reset,
};

void
avr_clkpr_init(avr_t * avr, avr_clkpr_t * p, avr_io_addr_t r_clkpr,
		uint8_t ckdiv8_fuse_byte, uint8_t ckdiv8_fuse_bit)
{
	memset(p, 0, sizeof(*p));
	p->io = _io;
	p->r_clkpr = r_clkpr;
	p->ckdiv8_fuse_byte = ckdiv8_fuse_byte;
	p->ckdiv8_fuse_bit = ckdiv8_fuse_bit;
	p->div = p->reset_div = 1;
	p->base_hz = avr->frequency;
	p->last_hz = avr->frequency;

	avr_register_io(avr, &p->io);
	avr_register_io_write(avr, r_clkpr, clkpr_write, p);
}
