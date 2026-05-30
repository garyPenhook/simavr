/*
	avr_clkctrl.h

	Modern AVR Clock Controller (CLKCTRL). The control registers are plain
	read/write storage; the one piece of behaviour modelled is MCLKSTATUS, which
	reports the selected clock source as running and stable so firmware that
	polls oscillator-ready / clock-switch status does not hang.

	As with the rest of simavr, the simulated CPU runs at the firmware's declared
	F_CPU; the CLKCTRL prescaler is not (yet) reflected back into avr->frequency.

	Copyright 2024 simavr modern-AVR fork (garyPenhook)

	This file is part of simavr. GNU GPL v3 or later; see COPYING.
 */

#ifndef __AVR_CLKCTRL_H__
#define __AVR_CLKCTRL_H__

#include "sim_avr.h"

#ifdef __cplusplus
extern "C" {
#endif

// register offsets from the CLKCTRL base
enum {
	AVR_CLKCTRL_MCLKCTRLA = 0x00,
	AVR_CLKCTRL_MCLKCTRLB = 0x01,
	AVR_CLKCTRL_MCLKLOCK = 0x02,
	AVR_CLKCTRL_MCLKSTATUS = 0x03,
};

// MCLKCTRLA.CLKSEL (bits 1:0) clock sources
#define AVR_CLKCTRL_CLKSEL_gm	0x03
// MCLKSTATUS bits
#define AVR_CLKCTRL_SOSC		(1 << 0)	// system oscillator changing
#define AVR_CLKCTRL_OSC20MS		(1 << 4)
#define AVR_CLKCTRL_OSC32KS		(1 << 5)
#define AVR_CLKCTRL_XOSC32KS	(1 << 6)
#define AVR_CLKCTRL_EXTS		(1 << 7)

typedef struct avr_clkctrl_t {
	avr_io_t		io;
	avr_io_addr_t	r_base;
} avr_clkctrl_t;

void avr_clkctrl_init(avr_t * avr, avr_clkctrl_t * p);

#ifdef __cplusplus
};
#endif

#endif /* __AVR_CLKCTRL_H__ */
