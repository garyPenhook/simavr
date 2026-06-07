/*
	avr_wdt.h

	"Modern" AVR (AVRxt) Watchdog Timer (WDT at 0x0100 on the tinyAVR 1-series,
	also megaAVR-0 and AVR Dx families).

	Unlike the classic WDT (WDTCSR + an optional interrupt), the modern WDT is
	reset-only: if it is not cleared by a WDR instruction within the configured
	period it resets the device. CTRLA selects the timeout PERIOD and an optional
	closed WINDOW; both fields are Configuration-Change-Protected, and
	STATUS.LOCK can make the configuration read-only until reset.

	This shares the WDR instruction with the classic model via the
	AVR_IOCTL_WATCHDOG_RESET ioctl. The watchdog clock is the 1.024 kHz
	OSCULP32K-derived clock, converted to CPU cycles via avr->frequency.

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

#ifndef __AVR_WDT_H__
#define __AVR_WDT_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "sim_avr.h"

/* Register offsets within a modern WDT block (device header WDT_t). */
enum {
	WDTR_CTRLA = 0x00,
	WDTR_STATUS = 0x01,
};

typedef struct avr_wdt_modern_t {
	avr_io_t	io;
	char		name;

	avr_io_addr_t	base;
	avr_io_addr_t	r_ctrla, r_status;

	uint32_t	timeout_cycles;	/* CPU cycles for the open period (0 = off) */
	uint32_t	window_cycles;	/* CPU cycles the window stays closed (0 = none) */
	avr_cycle_count_t	armed_at;	/* cycle the current period started */

	struct {
		uint8_t		pending;	/* a watchdog reset is being performed */
		avr_run_t	avr_run;	/* saved run callback, restored on reset */
	} reset_context;

	void (*reset_cb)(avr_t * avr, void * param);
	void *reset_param;
} avr_wdt_modern_t;

/*
 * Initialise a modern WDT block at data address 'base'. 'name' is a tag for
 * debug.
 */
void
avr_wdt_modern_init(
		avr_t * avr,
		avr_wdt_modern_t * p,
		avr_io_addr_t base,
		char name);

void
avr_wdt_modern_set_reset_handler(
		avr_wdt_modern_t * p,
		void (*cb)(avr_t * avr, void * param),
		void * param);

#ifdef __cplusplus
};
#endif

#endif /* __AVR_WDT_H__ */
