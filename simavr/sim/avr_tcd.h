/*
	avr_tcd.h

	"Modern" AVR (AVRxt) 12-bit Timer/Counter type D (TCD0 at 0x0A80 on the
	tinyAVR 1-series, also AVR Dx). TCD is an asynchronous timer aimed at
	high-resolution PWM; this models its periodic-overflow use together with the
	four waveform-generation modes.

	The counter runs from the selected TCD clock source (CTRLA.CLKSEL: the
	unprescaled OSC20M, or the System Clock CLK_PER) divided by SYNCPRES then
	CNTPRES. Because the source can differ from CLK_PER, a prescaled main clock
	makes an OSC20M-clocked TCD run faster than the CPU; the model scales the
	schedule by CLK_PER / f_TCD accordingly. In one-ramp operation it completes a
	cycle every (CMPBCLR + 1) counts, at which point the OVF flag is set and
	TCD0_OVF raised (if enabled). The double-
	buffered enable/command protocol is satisfied by reporting STATUS.ENRDY and
	CMDRDY always ready, so the usual `while (!(TCD0.STATUS & ENRDY))` polling
	passes.

	In One Ramp, Two Ramp, Four Ramp, and Dual Slope modes, the compare values
	drive the two waveform outputs using the datasheet ramp ordering. Each output
	is published on its WOA/WOB IRQ as it toggles, but only when enabled in
	FAULTCTRL (CMPAEN/CMPBEN) — so a board/test can observe the generated PWM.

	Not modelled: TRIGA/TRIGB compare events, dithering, fault input, input
	capture, the EXTCLK source (no external-clock pin), and the dedicated TCD/PLL
	clock; those registers still store. The OSC20M and SYSCLK sources are modelled
	(CLKSEL), but sub-CLK_PER count resolution is not representable so the
	per-count schedule is rounded to whole CLK_PER cycles.

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

#ifndef __AVR_TCD_H__
#define __AVR_TCD_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "sim_avr.h"

/* Register offsets within a TCD block (device header TCD_t). */
enum {
	TCDR_CTRLA = 0x00,
	TCDR_CTRLB = 0x01,
	TCDR_CTRLC = 0x02,	/* CMPCSEL / CMPDSEL select the PWM shown on WOC / WOD */
	TCDR_CTRLE = 0x04,
	TCDR_EVCTRLA = 0x08,	/* TRIGEI, EDGE for event input A */
	TCDR_EVCTRLB = 0x09,
	TCDR_INPUTCTRLA = 0x10,	/* INPUTMODE for event input A (AVR Dx) */
	TCDR_INPUTCTRLB = 0x11,
	TCDR_DLYCTRL = 0x14,	/* input blanking / programmable event (AVR Dx) */
	TCDR_DLYVAL = 0x15,
	TCDR_DITCTRL = 0x18,	/* dithering (AVR Dx) */
	TCDR_DITVAL = 0x19,
	TCDR_DBGCTRL = 0x1e,
	TCDR_CAPTUREAL = 0x22,	/* CAPTUREA L/H, CAPTUREB L/H (AVR Dx) */
	TCDR_CAPTUREBL = 0x24,
	TCDR_INTCTRL = 0x0c,
	TCDR_INTFLAGS = 0x0d,
	TCDR_STATUS = 0x0e,
	TCDR_FAULTCTRL = 0x12,	/* CMPAEN/CMPBEN output enables */
	TCDR_CMPASETL = 0x28,	/* 16-bit (12-bit value) compares */
	TCDR_CMPACLRL = 0x2a,
	TCDR_CMPBSETL = 0x2c,
	TCDR_CMPBCLRL = 0x2e,	/* TOP */
	TCDR_CMPBCLRH = 0x2f,
};

/*
 * IRQs: the waveform outputs WOA/WOB and, on parts that fit them, WOC/WOD (a copy
 * of WOA or WOB chosen by CTRLC.CMPCSEL / CMPDSEL, enabled by FAULTCTRL.CMPCEN /
 * CMPDEN); the EVSYS generator pulses (1 then 0) when the counter matches
 * CMPBCLR / CMPASET / CMPBSET; and the INPUT A / INPUT B event levels, which the
 * board or EVSYS drives (EVCTRLx.TRIGEI / EDGE select the active edge).
 */
enum {
	AVR_TCD_IRQ_WOA = 0,
	AVR_TCD_IRQ_WOB,
	AVR_TCD_IRQ_WOC,
	AVR_TCD_IRQ_WOD,
	AVR_TCD_IRQ_EVT_CMPBCLR,
	AVR_TCD_IRQ_EVT_CMPASET,
	AVR_TCD_IRQ_EVT_CMPBSET,
	AVR_TCD_IRQ_IN_A,
	AVR_TCD_IRQ_IN_B,
	AVR_TCD_IRQ_EVT_PROGEV,	/* programmable output event (AVR Dx, EVSYS generator 0xB3) */
	AVR_TCD_IRQ_COUNT,
};

typedef struct avr_tcd_t {
	avr_io_t	io;
	char		name;

	avr_io_addr_t	base;
	avr_io_addr_t	r_ctrla, r_ctrlb, r_intctrl, r_intflags, r_status;
	avr_io_addr_t	r_faultctrl, r_cmpaset, r_cmpaclr, r_cmpbset, r_cmpbclr;
	avr_io_addr_t	r_ctrlc, r_evctrla;

	avr_int_vector_t	ovf;	/* TCDn_OVF */
	avr_int_vector_t	trig;	/* TCDn_TRIG (AVR Dx): INTFLAGS.TRIGA / TRIGB */

	/* Optional CLKSEL source resolver (AVR Dx: CLKSEL 0 = OSCHF, 1 = PLL, 2 =
	 * EXTCLK, 3 = SYSCLK). When set it supplies f_TCD in Hz for CLKSEL 0..2
	 * (0 = unknown: track CLK_PER); freq_osc20m is then unused. */
	uint32_t		(*clk_fn)(void * param, uint8_t clksel);
	void *			clk_param;
	uint32_t		freq_osc20m;	/* unprescaled OSC20M, for CLKSEL=OSC20M */

	avr_cycle_count_t	start_cycle;
	int32_t			start_count;
	uint32_t		prescale;	/* CLK_PER cycles per TCD count */
	uint32_t		top;		/* CMPBCLR captured at start */
	uint8_t			phase;
	int8_t			dir;

	int			base_irq;
	uint8_t		woa, wob;	/* last published output levels */
	uint8_t		woa_raw, wob_raw;	/* waveform levels whether or not enabled */
	uint8_t		woc, wod;	/* published WOC / WOD levels */
	uint8_t		in_a, in_b;	/* event input levels (edge detection) */

	/* AVR Dx extras, enabled by avr_tcd_set_trig_vector(): input modes, capture,
	 * input blanking and the programmable event output. */
	uint8_t		ext;
	uint8_t		woa_nat, wob_nat;	/* waveform before any input-mode override */
	uint8_t		act_a, act_b;		/* input is at its active level (TRIGEI set) */
	uint8_t		blk_a, blk_b;		/* output blocked for the rest of the on-time */
	uint8_t		stopped;		/* input mode 7: counter halted until RESTART */
	uint8_t		blanking;		/* input A blanking window open */
	uint8_t		cap_lock_a, cap_lock_b;	/* capture held until the high byte is read */
	/* Counter-flow input modes (1-3, 5, 6, 8). Segments: 0 DTA, 1 OTA, 2 DTB, 3 OTB. */
	uint8_t		waiting;		/* counter halted until the input is released */
	uint8_t		wait_all;		/* ... and both outputs forced (mode 6) */
	uint8_t		wait_input;		/* input (0 = A, 1 = B) whose release resumes */
	uint8_t		resume_seg;		/* segment started on release */
	uint8_t		pend6;			/* mode 6: wait at the next on-time start */
	uint8_t		arm_mode, arm_input;	/* mode 2 / 3 armed for 'arm_input' */
	uint8_t		hold_a, hold_b;		/* output held at the fault level */
	uint8_t		jumped, in_tick;			/* the counter was repositioned inside a tick */
	uint8_t		dith_acc, dith_done;	/* dither accumulator, cycle-end hold done */
	uint8_t		dbg_halted;
	uint8_t		filt_level[2];		/* digital filter: level being qualified */
	uint8_t		line[2];		/* current level of each event input */
	uint8_t		sync_level[2];		/* last level queued into the synchroniser */
	uint8_t		raw_prev[2];		/* last level seen by the asynchronous path */
	uint8_t		sq_n[2], sq_head[2];	/* synchroniser FIFO (events are delivered in order) */
	uint8_t		sq_lvl[2][8];
	avr_cycle_count_t	sq_due[2][8];
} avr_tcd_t;

/*
 * Initialise a TCD block at data address 'base'. 'vec_ovf' is the TCDn_OVF
 * vector; 'name' is a tag for debug. 'osccfg_fuse_index' is the OSCCFG fuse
 * index (0xff if none), used to resolve the OSC20M base frequency for the
 * CLKSEL=OSC20M clock source.
 */
void
avr_tcd_init(
		avr_t * avr,
		avr_tcd_t * p,
		avr_io_addr_t base,
		uint8_t vec_ovf,
		uint8_t osccfg_fuse_index,
		char name);

/*
 * Install the TCDn_TRIG interrupt (AVR Dx): INTCTRL.TRIGA/TRIGB enable it and
 * INTFLAGS.TRIGA/TRIGB flag an active edge on event input A/B while the
 * corresponding EVCTRLx.TRIGEI is set (DS40002413B 25.3.5). It also enables the
 * Dx extras: software / event capture, input modes 0, 4, 7, 9 and 10, input
 * blanking and PROGEV. The counter-flow input modes 1-3, 5, 6 and 8, the digital
 * filter / asynchronous detection and dithering are not modelled.
 */
void
avr_tcd_set_trig_vector(
		avr_t * avr,
		avr_tcd_t * p,
		uint8_t vec_trig);

/*
 * Tell the TCD the CPU entered (halted = 1) or left Debug break. simavr has no
 * debug-halt notification of its own, so the embedder / debugger front end calls
 * this. DBGCTRL.DBGRUN = 0 freezes the timer and ignores events during the break;
 * DBGCTRL.FAULTDET = 1 raises a Fault on both inputs for as long as the break lasts
 * and sets TRIGA / TRIGB (DS40002413B 25.3.7). Call it from the simulation thread
 * (the one running avr_run()). The GDB stub and sim_host call it for every TCD
 * through avr_debug_halt() / avr_io_t.debug_halt.
 */
void
avr_tcd_debug_halt(
		avr_t * avr,
		avr_tcd_t * p,
		int halted);

#define AVR_IOCTL_TCD_GETIRQ(_name) AVR_IOCTL_DEF('t','c','d',(_name))

#ifdef __cplusplus
};
#endif

#endif /* __AVR_TCD_H__ */
