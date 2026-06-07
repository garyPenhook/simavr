/*
	avr_tcb.h

	"Modern" AVR (AVRxt) 16-bit Timer/Counter type B (TCB), as found on the
	tinyAVR 1-series (ATtiny3217), megaAVR-0 and AVR Dx families.

	This models the common TCB operating modes used on modern AVRs:
	  - Periodic Interrupt
	  - Time-Out Check
	  - Input Capture on Event
	  - Input Capture Frequency Measurement
	  - Input Capture Pulse-Width Measurement
	  - Input Capture Frequency and Pulse-Width Measurement
	  - 8-Bit PWM (continuous waveform output on WO)

	The counter is driven from CLK_PER (optionally /2) using simavr cycle timers;
	CNT reads return a computed live value. Event-driven modes consume an event
	input IRQ and publish a capture-event IRQ that can be routed into EVSYS. In
	8-bit PWM mode the WO level is published on its output IRQ as it toggles, so
	it can be routed into the CCL input MUX. Single-Shot mode (the other
	WO-producing mode) is not modelled yet; its WO stays low.

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

#ifndef __AVR_TCB_H__
#define __AVR_TCB_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "sim_avr.h"

/* Register offsets within a TCB block (device header TCB_t). */
enum {
	TCBR_CTRLA = 0x00,
	TCBR_CTRLB = 0x01,
	TCBR_EVCTRL = 0x04,
	TCBR_INTCTRL = 0x05,
	TCBR_INTFLAGS = 0x06,
	TCBR_STATUS = 0x07,
	TCBR_DBGCTRL = 0x08,
	TCBR_TEMP = 0x09,
	TCBR_CNTL = 0x0a,
	TCBR_CNTH = 0x0b,
	TCBR_CCMPL = 0x0c,
	TCBR_CCMPH = 0x0d,
};

typedef struct avr_tcb_t {
	avr_io_t	io;
	char		name;		// '0', '1', …

	avr_io_addr_t	base;
	avr_io_addr_t	r_ctrla, r_ctrlb, r_evctrl, r_intctrl, r_intflags, r_status;
	avr_io_addr_t	r_cnt, r_ccmp;	// 16-bit (low byte address)

	avr_int_vector_t	vect;	// TCBn_INT (CAPT)

	/* Running-counter bookkeeping (for computed CNT and rescheduling). */
	avr_cycle_count_t	start_cycle;	// cycle at which counting last resumed
	uint32_t		prescale;	// CPU cycles per timer tick (1 or 2)
	uint32_t		top;		// CCMP captured when the timer started
	uint16_t		start_count;	// CNT value at start_cycle
	uint16_t		freeze_count;	// stationary CNT when not running

	uint8_t			event_level;
	uint8_t			pw_armed;
	uint8_t			frqpw_stage;
	uint8_t			wo_level;	// last WO level published on its IRQ (PWM8)
	int			base_irq;
} avr_tcb_t;

enum {
	AVR_TCB_IRQ_EVENT_IN = 0,
	AVR_TCB_IRQ_CAPT_OUT,
	AVR_TCB_IRQ_WO,		// 8-bit PWM waveform output level
	AVR_TCB_IRQ_COUNT,
};

#define AVR_IOCTL_TCB_GETIRQ(_name) AVR_IOCTL_DEF('t','c','b',(_name))

/*
 * Initialise a TCB block at data address 'base'. 'vector' is the TCBn_INT
 * interrupt vector number; 'name' is a tag for debug.
 */
void
avr_tcb_init(
		avr_t * avr,
		avr_tcb_t * p,
		avr_io_addr_t base,
		uint8_t vector,
		char name);

#ifdef __cplusplus
};
#endif

#endif /* __AVR_TCB_H__ */
