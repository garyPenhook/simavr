/*
	avr_tca.h

	"Modern" AVR (AVRxt) 16-bit Timer/Counter type A (TCA), as found on the
	tinyAVR 1-series (ATtiny3217), megaAVR-0 and AVR Dx families.

	Single (16-bit) mode: a prescaled counter with TOP = PER (or CMP0 in FRQ),
	the overflow interrupt (OVF) and the three compare-match interrupts
	(CMP0/1/2). The counter is advanced by a simavr cycle timer scheduled to the
	next interesting count (a compare match, TOP or BOTTOM), so there is no
	per-cycle cost. All waveform generation modes of DS40002205A table 20-6 are
	modelled: NORMAL, FRQ (toggle on CMPn match, period CMP0+1), SINGLESLOPE and
	the three dual-slope modes (DSTOP / DSBOTH / DSBOTTOM, which differ in where
	OVF is raised). PER and CMP0-2 are double buffered (PERBUF / CMPnBUF, buffer
	valid flags in CTRLF, CTRLE.LUPD and CTRLB.ALUPD); the buffers transfer on
	the UPDATE condition (wrap / BOTTOM) or on a CTRLE UPDATE command. CTRLE
	also carries the RESTART / RESET commands and the software direction bit
	(DIR); CTRLC.CMPnOV drives WOn while the timer is disabled.

	Each enabled channel (CMPnEN in CTRLB) publishes its WOn level on an output
	IRQ as it changes, so it can be routed into the CCL input MUX and PORTMUX.

	Split mode (CTRLD.SPLITM) is modelled as two independent 8-bit
	down-counters (LCNT/HCNT, LPER/HPER, LCMPn/HCMPn) with six PWM outputs
	WO0-5 (WO3-5 are the high-byte channels), LUNF/HUNF underflow and LCMPn
	interrupts. The data sheet does not give the split-mode PWM polarity; the
	output is modelled high while CNT < CMPn, so duty = CMPn / (PER + 1), as for
	single-slope.

	Event counting (EVCTRL.CNTEI + EVACT) is modelled. The EVSYS user that
	routes to TCA0 (SYNCUSER0) drives the AVR_TCA_IRQ_EV_IN input; the four
	EVACT modes (DS40002205A 20.5.9) are handled as:
	  * POSEDGE / ANYEDGE — the counter is clocked by the event, not the
	    prescaled clock: each qualifying edge advances CNT by one count (with
	    compare/overflow/WO handling). The prescaled-clock scheduler is
	    suspended while in these modes.
	  * HIGHLVL — the prescaled clock counts only while the event line is high
	    (a clock gate); the scheduler runs/freezes with the level.
	  * UPDOWN — the prescaled clock counts up while the event line is low and
	    down while it is high.

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
	TCAR_CTRLECLR = 0x04,
	TCAR_CTRLESET = 0x05,
	TCAR_CTRLFCLR = 0x06,
	TCAR_CTRLFSET = 0x07,
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
	TCAR_PERBUFL = 0x36,
	TCAR_CMP0BUFL = 0x38,
	TCAR_CMP1BUFL = 0x3a,
	TCAR_CMP2BUFL = 0x3c,
};

typedef struct avr_tca_t {
	avr_io_t	io;
	char		name;

	avr_io_addr_t	base;
	avr_io_addr_t	r_ctrla, r_ctrlb, r_evctrl, r_intctrl, r_intflags;
	avr_io_addr_t	r_cnt, r_per, r_cmp[3];

	avr_int_vector_t	ovf;		// TCA0_OVF
	avr_int_vector_t	cmp[3];		// TCA0_CMP0/1/2 (LCMP0/1/2 in split mode)
	avr_int_vector_t	hunf;		// TCA0_HUNF (split mode only; OVF vector + 1)

	/* Running-counter bookkeeping. */
	avr_cycle_count_t	start_cycle;	// cycle at which CNT == 0
	uint32_t		prescale;	// CPU cycles per timer tick
	uint32_t		ev_target;	// CNT value of the currently-scheduled event
	uint8_t			clock_running;	// 1 while the prescaled-clock timer drives CNT
	uint8_t			ev_input;	// last event-line level seen on EV_IN

	uint8_t			lupd;		// CTRLE.LUPD (lock update)
	uint8_t			dir_sw;		// CTRLE.DIR as last written by software
	uint8_t			dir_down;	// effective count direction (1 = down)
	uint32_t		ev_phase;	// phase (ticks into the period) of the scheduled event
	uint8_t			frq_lvl[3];	// FRQ waveform state (toggled on CMPn match)

	/* Split mode: one down-counter per half (0 = low byte, 1 = high byte). */
	struct avr_tca_half_t {
		struct avr_tca_t	*p;
		int			h;
		avr_cycle_count_t	start_cycle;	// cycle at which phase == 0 (CNT == TOP)
		uint32_t		ev_phase;
		uint8_t			running;
	} half[2];

	uint8_t			wo_level[6];	// last WO0-5 level published on its IRQ
	uint8_t			pin_state[6];	// last PINn value (override<<1 | level) published
	int			base_irq;
} avr_tca_t;

/*
 * IRQs: WO0-5 are waveform-output levels (WO0-2 per compare channel; WO3-5 are the
 * split-mode high-byte channels). EV_IN is the event-line input (EVSYS SYNCUSER0 routes here) that
 * drives event counting per EVCTRL. PIN0/1/2 describe what the compare channel
 * does to its port pin: bit 0 = waveform level, bit 1 = the channel overrides
 * the pin (CMPnEN set and a waveform generation mode selected, DS40002205A
 * 20.3.3.4.5). Raised whenever either changes; PORTMUX routes it to a pin.
 */
enum {
	AVR_TCA_IRQ_WO0 = 0,
	AVR_TCA_IRQ_WO1,
	AVR_TCA_IRQ_WO2,
	AVR_TCA_IRQ_EV_IN,
	AVR_TCA_IRQ_PIN0,
	AVR_TCA_IRQ_PIN1,
	AVR_TCA_IRQ_PIN2,
	AVR_TCA_IRQ_WO3,		/* split mode, high-byte channels */
	AVR_TCA_IRQ_WO4,
	AVR_TCA_IRQ_WO5,
	AVR_TCA_IRQ_PIN3,
	AVR_TCA_IRQ_PIN4,
	AVR_TCA_IRQ_PIN5,
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
