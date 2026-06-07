/*
	avr_tca.h

	"Modern" AVR (AVRxt) 16-bit Timer/Counter type A (TCA), as found on the
	tinyAVR 1-series (ATtiny3217), megaAVR-0 and AVR Dx families.

	This models single (16-bit) mode: a prescaled up-counter with TOP = PER,
	the overflow interrupt (OVF) and the three compare-match interrupts
	(CMP0/1/2). The counter is advanced by a simavr cycle timer scheduled to the
	next interesting count (a compare value or the wrap), so there is no
	per-cycle cost.

	Single-slope PWM waveform output (WGMODE = SINGLESLOPE) is modelled: each
	enabled channel (CMPnEN in CTRLB) publishes its WOn level on an output IRQ
	as it toggles, so it can be routed into the CCL input MUX. FRQ and the
	dual-slope WGMODE variants are not (the engine is a single-slope up-counter:
	FRQ uses TOP = CMP0 and dual-slope down-counts, neither of which it tracks),
	so WOn stays low in those modes. Split (dual 8-bit) mode is not modelled (the
	registers still store).

	Event counting (EVCTRL.CNTEI + EVACT) is modelled. The EVSYS user that
	routes to TCA0 (SYNCUSER0) drives the AVR_TCA_IRQ_EV_IN input; the four
	EVACT modes (DS40002205A 20.5.10) are handled as:
	  * POSEDGE / ANYEDGE — the counter is clocked by the event, not the
	    prescaled clock: each qualifying edge advances CNT by one count (with
	    compare/overflow/WO handling). The prescaled-clock scheduler is
	    suspended while in these modes.
	  * HIGHLVL — the prescaled clock counts only while the event line is high
	    (a clock gate); the scheduler runs/freezes with the level.
	  * UPDOWN — the prescaled clock counts up while the event line is low; the
	    down-count half (event high) is not representable by the single-slope
	    up-counter, so the counter freezes there (documented gap).

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

#ifndef __AVR_TCA_H__
#define __AVR_TCA_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "sim_avr.h"

/* Register offsets within a TCA block, single (16-bit) view (device TCA_t). */
enum {
	TCAR_CTRLA = 0x00,
	TCAR_CTRLB = 0x01,
	TCAR_CTRLC = 0x02,
	TCAR_CTRLD = 0x03,
	TCAR_EVCTRL = 0x09,
	TCAR_INTCTRL = 0x0a,
	TCAR_INTFLAGS = 0x0b,
	TCAR_CNTL = 0x20,
	TCAR_CNTH = 0x21,
	TCAR_PERL = 0x26,
	TCAR_PERH = 0x27,
	TCAR_CMP0L = 0x28,
	TCAR_CMP1L = 0x2a,
	TCAR_CMP2L = 0x2c,
};

typedef struct avr_tca_t {
	avr_io_t	io;
	char		name;

	avr_io_addr_t	base;
	avr_io_addr_t	r_ctrla, r_ctrlb, r_evctrl, r_intctrl, r_intflags;
	avr_io_addr_t	r_cnt, r_per, r_cmp[3];

	avr_int_vector_t	ovf;		// TCA0_OVF
	avr_int_vector_t	cmp[3];		// TCA0_CMP0/1/2

	/* Running-counter bookkeeping. */
	avr_cycle_count_t	start_cycle;	// cycle at which CNT == 0
	uint32_t		prescale;	// CPU cycles per timer tick
	uint32_t		ev_target;	// CNT value of the currently-scheduled event
	uint8_t			clock_running;	// 1 while the prescaled-clock timer drives CNT
	uint8_t			ev_input;	// last event-line level seen on EV_IN

	uint8_t			wo_level[3];	// last WO0/1/2 level published on its IRQ
	int			base_irq;
} avr_tca_t;

/*
 * IRQs: WO0/1/2 are single-slope PWM waveform-output levels (one per compare
 * channel). EV_IN is the event-line input (EVSYS SYNCUSER0 routes here) that
 * drives event counting per EVCTRL.
 */
enum {
	AVR_TCA_IRQ_WO0 = 0,
	AVR_TCA_IRQ_WO1,
	AVR_TCA_IRQ_WO2,
	AVR_TCA_IRQ_EV_IN,
	AVR_TCA_IRQ_COUNT,
};

#define AVR_IOCTL_TCA_GETIRQ(_name) AVR_IOCTL_DEF('t','c','a',(_name))

/*
 * Initialise a TCA block at data address 'base'. The OVF and CMP0..2 interrupt
 * vector numbers come from the device header; 'name' is a debug tag.
 */
void
avr_tca_init(
		avr_t * avr,
		avr_tca_t * p,
		avr_io_addr_t base,
		uint8_t vec_ovf,
		uint8_t vec_cmp0,
		uint8_t vec_cmp1,
		uint8_t vec_cmp2,
		char name);

#ifdef __cplusplus
};
#endif

#endif /* __AVR_TCA_H__ */
