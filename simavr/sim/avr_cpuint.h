/*
	avr_cpuint.h

	Modern AVR CPUINT interrupt-controller register block. The actual dispatch
	logic lives in the core (sim_interrupts.c, AVR_ARCH_F_CPUINT); this module
	just maps the CPUINT.CTRLA / STATUS / LVL0PRI / LVL1VEC registers onto the
	engine's avr_cpuint_set_* / avr_cpuint_get_status API.

	Copyright 2024 simavr modern-AVR fork (garyPenhook)

	This file is part of simavr. GNU GPL v3 or later; see COPYING.
 */

#ifndef __AVR_CPUINT_H__
#define __AVR_CPUINT_H__

#include "sim_avr.h"

#ifdef __cplusplus
extern "C" {
#endif

// register offsets from the CPUINT base
enum {
	AVR_CPUINT_R_CTRLA = 0,
	AVR_CPUINT_R_STATUS,
	AVR_CPUINT_R_LVL0PRI,
	AVR_CPUINT_R_LVL1VEC,
};

#define AVR_CPUINT_CTRLA_LVL0RR	(1 << 0)	// round-robin enable

typedef struct avr_cpuint_t {
	avr_io_t		io;
	avr_io_addr_t	r_base;		// data address of CPUINT (CTRLA)
} avr_cpuint_t;

void avr_cpuint_init(avr_t * avr, avr_cpuint_t * p);

#ifdef __cplusplus
};
#endif

#endif /* __AVR_CPUINT_H__ */
