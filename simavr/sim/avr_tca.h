/*
	avr_tca.h

	Modern AVR 16-bit Timer/Counter type A (TCA), Normal mode (WGMODE=NORMAL).
	The counter runs on CLK_PER with the CLKSEL prescale from 0 up to PER, then
	overflows (OVF) and wraps; compare matches on CMP0/CMP1/CMP2 raise their
	interrupts. Split mode and the waveform-output (PWM) pin drive are not yet
	modelled (PWM needs PORTMUX/port override).

	Copyright 2024 simavr modern-AVR fork (garyPenhook)

	This file is part of simavr. GNU GPL v3 or later; see COPYING.
 */

#ifndef __AVR_TCA_H__
#define __AVR_TCA_H__

#include "sim_avr.h"
#include "sim_interrupts.h"

#ifdef __cplusplus
extern "C" {
#endif

// register offsets from the TCA base (SINGLE / normal-mode layout)
enum {
	AVR_TCA_CTRLA = 0x00, AVR_TCA_CTRLB = 0x01, AVR_TCA_CTRLD = 0x03,
	AVR_TCA_INTCTRL = 0x0A, AVR_TCA_INTFLAGS = 0x0B,
	AVR_TCA_CNT = 0x20, AVR_TCA_PER = 0x26,
	AVR_TCA_CMP0 = 0x28, AVR_TCA_CMP1 = 0x2A, AVR_TCA_CMP2 = 0x2C,
};

#define AVR_TCA_CTRLA_ENABLE	(1 << 0)
#define AVR_TCA_CTRLA_CLKSEL_gp	1
#define AVR_TCA_CTRLA_CLKSEL_gm	(7 << 1)
#define AVR_TCA_CTRLD_SPLITM	(1 << 0)
#define AVR_TCA_INT_OVF			(1 << 0)
#define AVR_TCA_INT_CMP0		(1 << 4)
#define AVR_TCA_INT_CMP1		(1 << 5)
#define AVR_TCA_INT_CMP2		(1 << 6)

typedef struct avr_tca_t {
	avr_io_t			io;
	char				name;		// '0'
	avr_io_addr_t		r_base;
	avr_int_vector_t	ovf, cmp0, cmp1, cmp2;

	avr_cycle_count_t	base_cycle;	// cycle the current period started
	uint32_t			prescaler;	// 1,2,4,8,16,64,256,1024
	uint32_t			period;		// (PER+1) * prescaler, in CPU cycles (0=off)
} avr_tca_t;

void avr_tca_init(avr_t * avr, avr_tca_t * p);

#ifdef __cplusplus
};
#endif

#endif /* __AVR_TCA_H__ */
