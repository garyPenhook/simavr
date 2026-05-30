/*
	avr_clkctrl.c

	Modern AVR Clock Controller. See avr_clkctrl.h.

	Copyright 2024 simavr modern-AVR fork (garyPenhook)

	This file is part of simavr. GNU GPL v3 or later; see COPYING.
 */

#include "avr_clkctrl.h"

/*
 * MCLKSTATUS reports the selected main clock source as running and stable
 * (SOSC=0 means "not in the middle of a switch"). This keeps firmware that
 * waits for oscillator readiness or clock-switch completion from spinning
 * forever.
 */
static uint8_t
avr_clkctrl_status_read(avr_t * avr, avr_io_addr_t addr, void * param)
{
	avr_clkctrl_t * p = (avr_clkctrl_t *)param;
	uint8_t clksel = avr->data[p->r_base + AVR_CLKCTRL_MCLKCTRLA] &
			AVR_CLKCTRL_CLKSEL_gm;
	uint8_t s = 0;	// SOSC cleared: the clock is not changing

	switch (clksel) {
		case 0: s |= AVR_CLKCTRL_OSC20MS; break;	// OSC20M (default)
		case 1: s |= AVR_CLKCTRL_OSC32KS; break;	// OSCULP32K
		case 2: s |= AVR_CLKCTRL_XOSC32KS; break;	// XOSC32K
		case 3: s |= AVR_CLKCTRL_EXTS; break;		// EXTCLK
	}
	return s;
}

void
avr_clkctrl_init(avr_t * avr, avr_clkctrl_t * p)
{
	p->io.kind = "clkctrl";
	avr_register_io(avr, &p->io);
	// control registers (MCLKCTRLA/B, MCLKLOCK, oscillator controls) behave as
	// plain storage; only MCLKSTATUS is derived.
	avr_register_io_read(avr, p->r_base + AVR_CLKCTRL_MCLKSTATUS,
			avr_clkctrl_status_read, p);
}
