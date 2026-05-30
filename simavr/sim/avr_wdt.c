/*
	avr_wdt.c

	"Modern" AVR (AVRxt) Watchdog Timer — reset-only. See avr_wdt.h.

	When enabled (CTRLA.PERIOD != OFF) a simavr cycle timer is armed for the
	timeout; a WDR instruction (delivered via AVR_IOCTL_WATCHDOG_RESET) re-arms
	it. If the timer expires first the device is reset. In windowed mode a WDR
	that arrives before the closed window has elapsed also resets the device.
	The reset is performed the same way as the classic watchdog: swap avr->run
	to a callback that calls avr_reset(), then restore it from the reset hook.

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
#include "avr_wdt.h"
#include "avr_watchdog.h"	/* AVR_IOCTL_WATCHDOG_RESET (shared WDR ioctl) */
#include "sim_cycle_timers.h"

/* CTRLA */
#define PERIOD_gm	0x0f
#define PERIOD_gp	0
#define WINDOW_gm	0xf0
#define WINDOW_gp	4

/* STATUS */
#define SYNCBUSY_bm	0x01
#define LOCK_bm		0x80

static inline uint8_t rd(avr_t *avr, avr_io_addr_t a) { return avr->data[a]; }

/* PERIOD/WINDOW field value -> WDT clock cycles (8 << (v-1)); 0 = off/none. */
static uint32_t wdt_field_cycles(avr_wdt_modern_t *p, uint8_t v)
{
	avr_t *avr = p->io.avr;
	if (v < 1 || v > 11)
		return 0;
	uint32_t wdt_cyc = 8u << (v - 1);	/* 8CLK .. 8KCLK at 1.024 kHz */
	uint64_t c = (uint64_t)wdt_cyc * avr->frequency / 1024;
	return c ? (uint32_t)c : 1;
}

static void wdt_recompute(avr_wdt_modern_t *p)
{
	avr_t *avr = p->io.avr;
	uint8_t ctrla = rd(avr, p->r_ctrla);
	p->timeout_cycles = wdt_field_cycles(p, (ctrla & PERIOD_gm) >> PERIOD_gp);
	p->window_cycles = wdt_field_cycles(p, (ctrla & WINDOW_gm) >> WINDOW_gp);
}

/* Run callback installed momentarily to perform the reset safely. */
static void wdt_do_reset(avr_t *avr)
{
	avr_reset(avr);
}

static void wdt_trigger_reset(avr_wdt_modern_t *p)
{
	avr_t *avr = p->io.avr;
	AVR_LOG(avr, LOG_TRACE, "WDT: timeout, resetting\n");
	p->reset_context.avr_run = avr->run;
	p->reset_context.pending = 1;
	avr->run = wdt_do_reset;
}

static avr_cycle_count_t
avr_wdt_modern_timer(struct avr_t *avr, avr_cycle_count_t when, void *param)
{
	avr_wdt_modern_t *p = (avr_wdt_modern_t *)param;
	(void)when;
	if (p->timeout_cycles)
		wdt_trigger_reset(p);
	return 0;
}

static void wdt_arm(avr_wdt_modern_t *p)
{
	avr_t *avr = p->io.avr;
	avr_cycle_timer_cancel(avr, avr_wdt_modern_timer, p);
	if (p->timeout_cycles) {
		p->armed_at = avr->cycle;
		avr_cycle_timer_register(avr, p->timeout_cycles, avr_wdt_modern_timer, p);
	}
}

static int wdt_locked(avr_wdt_modern_t *p)
{
	return (rd(p->io.avr, p->r_status) & LOCK_bm) != 0;
}

static void
avr_wdt_modern_ctrla_write(struct avr_t *avr, avr_io_addr_t addr, uint8_t v,
						   void *param)
{
	avr_wdt_modern_t *p = (avr_wdt_modern_t *)param;
	/* CTRLA is configuration-change protected and frozen once STATUS.LOCK. */
	if (wdt_locked(p) || !avr_ccp_io_write_enabled(avr))
		return;
	avr_core_watch_write(avr, addr, v);
	wdt_recompute(p);
	wdt_arm(p);
}

static void
avr_wdt_modern_status_write(struct avr_t *avr, avr_io_addr_t addr, uint8_t v,
							void *param)
{
	(void)param;
	/* SYNCBUSY is read-only; LOCK is write-1 sticky (cleared only by reset). */
	uint8_t cur = avr->data[addr] & LOCK_bm;
	avr_core_watch_write(avr, addr, cur | (v & LOCK_bm));
}

static uint8_t
avr_wdt_modern_status_read(struct avr_t *avr, avr_io_addr_t addr, void *param)
{
	(void)param;
	return avr->data[addr] & LOCK_bm;	/* SYNCBUSY never busy */
}

/* WDR instruction: pet the dog (or reset if it is still inside the window). */
static int
avr_wdt_modern_ioctl(struct avr_io_t *io, uint32_t ctl, void *io_param)
{
	avr_wdt_modern_t *p = (avr_wdt_modern_t *)io;
	avr_t *avr = p->io.avr;
	(void)io_param;

	if (ctl != AVR_IOCTL_WATCHDOG_RESET)
		return -1;
	if (!p->timeout_cycles)
		return 0;	/* watchdog off: WDR is a no-op */

	if (p->window_cycles &&
		(avr->cycle - p->armed_at) < p->window_cycles) {
		/* Too early: a WDR inside the closed window resets the device. */
		wdt_trigger_reset(p);
		return 0;
	}
	wdt_arm(p);	/* re-start the timeout */
	return 0;
}

static void
avr_wdt_modern_reset(avr_io_t *io)
{
	avr_wdt_modern_t *p = (avr_wdt_modern_t *)io;
	avr_t *avr = p->io.avr;

	if (p->reset_context.pending) {
		p->reset_context.pending = 0;
		avr->run = p->reset_context.avr_run;	/* restore normal execution */
	}
	avr_cycle_timer_cancel(avr, avr_wdt_modern_timer, p);
	p->timeout_cycles = 0;
	p->window_cycles = 0;
	p->armed_at = 0;
}

static avr_io_t _io = {
	.kind = "wdt",
	.reset = avr_wdt_modern_reset,
	.ioctl = avr_wdt_modern_ioctl,
};

void
avr_wdt_modern_init(
		avr_t * avr,
		avr_wdt_modern_t * p,
		avr_io_addr_t base,
		char name)
{
	memset(p, 0, sizeof(*p));
	p->io = _io;
	p->name = name;
	p->base = base;
	p->r_ctrla = base + WDTR_CTRLA;
	p->r_status = base + WDTR_STATUS;

	avr_register_io(avr, &p->io);
	avr_register_io_write(avr, p->r_ctrla, avr_wdt_modern_ctrla_write, p);
	avr_register_io_write(avr, p->r_status, avr_wdt_modern_status_write, p);
	avr_register_io_read(avr, p->r_status, avr_wdt_modern_status_read, p);
}
