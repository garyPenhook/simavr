/*
	sim_tiny3217.h

	ATtiny3217 (tinyAVR 1-series, AVRxt core) descriptor for simavr.

	This is the first modern-AVR core. Unlike the classic cores it does not lean
	on the avr-libc io header for register addresses (the modern header is
	struct-based); the few addresses needed are taken directly from the
	datasheet (DS40002205A) and spelled out here. Engine support for the modern
	memory/IO model, AVRxt timing, CCP and the CPUINT interrupt controller lives
	behind the avr->arch.flags set below.

	Copyright 2024 simavr modern-AVR fork (garyPenhook)

	This file is part of simavr. GNU GPL v3 or later; see COPYING.
 */

#ifndef __SIM_TINY3217_H__
#define __SIM_TINY3217_H__

#include "sim_avr.h"
#include "avr_port.h"
#include "avr_cpuint.h"
#include "avr_tcb.h"
#include "avr_usart.h"

// --- ATtiny3217 memory map (data space) ---
#define T3217_FLASHEND	0x7FFF		// 32 KB
#define T3217_RAMSTART	0x3800		// 2 KB SRAM at 0x3800..0x3FFF
#define T3217_RAMEND	0x3FFF
#define T3217_E2SIZE	256			// EEPROM bytes
#define T3217_FLASHMAP	0x8000		// flash mapped into data space here

// CPU registers (modern layout)
#define T3217_CCP	0x0034
#define T3217_SPL	0x003D
#define T3217_SREG	0x003F

#define T3217_RSTFR	0x0040		// RSTCTRL.RSTFR (reset flags)
#define T3217_CPUINT	0x0110		// CPUINT controller
#define T3217_PORTA	0x0400
#define T3217_PORTB	0x0420
#define T3217_PORTC	0x0440
#define T3217_USART0	0x0800
#define T3217_TCB0	0x0A40
#define T3217_TCB1	0x0A50

void t3217_init(struct avr_t * avr);
void t3217_reset(struct avr_t * avr);

struct mcu_t {
	avr_t		core;
	avr_cpuint_t	cpuint;
	avr_port_t	porta, portb, portc;
	avr_usart_t	usart0;
	avr_tcb_t	tcb0, tcb1;
};

#ifdef SIM_CORENAME

#ifndef SIM_VECTOR_SIZE
#error SIM_VECTOR_SIZE is not declared
#endif
#ifndef SIM_MMCU
#error SIM_MMCU is not declared
#endif

const struct mcu_t SIM_CORENAME = {
	.core = {
		.mmcu = SIM_MMCU,
		.ioend = T3217_RAMSTART - 1,
		.ramend = T3217_RAMEND,
		.flashend = T3217_FLASHEND,
		.e2end = T3217_E2SIZE - 1,
		.vector_size = SIM_VECTOR_SIZE,
		.signature = { 0x1E, 0x95, 0x22 },
		.lockbits = 0xFF,

		.arch = {
			.flags = AVR_ARCH_F_MODERN | AVR_ARCH_F_CCP |
					 AVR_ARCH_F_XT_TIMING | AVR_ARCH_F_CPUINT,
			.io_offset = 0,
			.sp_addr = T3217_SPL,
			.sreg_addr = T3217_SREG,
			.ccp_addr = T3217_CCP,
			.flashmap_start = T3217_FLASHMAP,
		},

		.reset_flags = {
			.porf  = AVR_IO_REGBIT(T3217_RSTFR, 0),
			.extrf = AVR_IO_REGBIT(T3217_RSTFR, 1),
			.borf  = AVR_IO_REGBIT(T3217_RSTFR, 2),
			.wdrf  = AVR_IO_REGBIT(T3217_RSTFR, 3),
		},

		.init = t3217_init,
		.reset = t3217_reset,
	},
	.cpuint = { .r_base = T3217_CPUINT },
	.porta = { .name = 'A', .r_base = T3217_PORTA },
	.portb = { .name = 'B', .r_base = T3217_PORTB },
	.portc = { .name = 'C', .r_base = T3217_PORTC },
	.usart0 = {
		.name = '0', .r_base = T3217_USART0,
		// STATUS at +0x04, CTRLA at +0x05; DREIF/TXCIF/RXCIF and the matching
		// interrupt-enable bits share positions 5/6/7.
		.rxc = {
			.vector = 27,	// USART0_RXC_vect_num
			.enable = AVR_IO_REGBIT(T3217_USART0 + 0x05, 7),
			.raised = AVR_IO_REGBIT(T3217_USART0 + 0x04, 7),
			.raise_sticky = 1,
		},
		.dre = {
			.vector = 28,	// USART0_DRE_vect_num
			.enable = AVR_IO_REGBIT(T3217_USART0 + 0x05, 5),
			.raised = AVR_IO_REGBIT(T3217_USART0 + 0x04, 5),
			.raise_sticky = 1,
		},
		.txc = {
			.vector = 29,	// USART0_TXC_vect_num
			.enable = AVR_IO_REGBIT(T3217_USART0 + 0x05, 6),
			.raised = AVR_IO_REGBIT(T3217_USART0 + 0x04, 6),
			.raise_sticky = 1,
		},
	},
	.tcb0 = {
		.name = '0', .r_base = T3217_TCB0,
		.capt = {
			.vector = 13,	// TCB0_INT_vect_num
			.enable = AVR_IO_REGBIT(T3217_TCB0 + 0x05, 0),	// INTCTRL.CAPT
			.raised = AVR_IO_REGBIT(T3217_TCB0 + 0x06, 0),	// INTFLAGS.CAPT
			.raise_sticky = 1,
		},
	},
	.tcb1 = {
		.name = '1', .r_base = T3217_TCB1,
		.capt = {
			.vector = 14,	// TCB1_INT_vect_num
			.enable = AVR_IO_REGBIT(T3217_TCB1 + 0x05, 0),
			.raised = AVR_IO_REGBIT(T3217_TCB1 + 0x06, 0),
			.raise_sticky = 1,
		},
	},
};

#endif /* SIM_CORENAME */

#endif /* __SIM_TINY3217_H__ */
