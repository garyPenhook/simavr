/*
	avr_tcb.h

	Modern AVR 16-bit Timer/Counter type B (TCB). This implements the common
	Periodic Interrupt mode (CNTMODE=INT): the counter runs on CLK_PER (with the
	CLKSEL prescale) up to CCMP, then sets the CAPT interrupt flag and restarts.
	Other TCB modes (timeout/capture/single-shot/PWM) are not yet modelled.

	Copyright 2024 simavr modern-AVR fork (garyPenhook)

	This file is part of simavr. GNU GPL v3 or later; see COPYING.
 */

#ifndef __AVR_TCB_H__
#define __AVR_TCB_H__

#include "sim_avr.h"
#include "sim_interrupts.h"

#ifdef __cplusplus
extern "C" {
#endif

// register offsets from the TCB base
enum {
	AVR_TCB_CTRLA = 0x00,
	AVR_TCB_CTRLB = 0x01,
	AVR_TCB_INTCTRL = 0x05,
	AVR_TCB_INTFLAGS = 0x06,
	AVR_TCB_STATUS = 0x07,
	AVR_TCB_CNT = 0x0A,		// 16-bit (CNT, CNT+1)
	AVR_TCB_CCMP = 0x0C,	// 16-bit (CCMP, CCMP+1)
};

#define AVR_TCB_CTRLA_ENABLE	(1 << 0)
#define AVR_TCB_CTRLA_CLKSEL_gp	1
#define AVR_TCB_CTRLA_CLKSEL_gm	(3 << 1)
#define AVR_TCB_CTRLB_CNTMODE_gm	(7 << 0)
#define AVR_TCB_INT_CAPT		(1 << 0)
#define AVR_TCB_INT_OVF			(1 << 1)

typedef struct avr_tcb_t {
	avr_io_t			io;
	char				name;		// '0', '1'
	avr_io_addr_t		r_base;
	avr_int_vector_t	capt;		// TCBn_INT

	avr_cycle_count_t	base_cycle;	// cycle the current period started
	uint32_t			period;		// current period in CPU cycles (0 = stopped)
	uint8_t				clkdiv;		// CLK_PER divider (1 or 2)
} avr_tcb_t;

void avr_tcb_init(avr_t * avr, avr_tcb_t * p);

#ifdef __cplusplus
};
#endif

#endif /* __AVR_TCB_H__ */
