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
	TCDR_CTRLE = 0x04,
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

/* IRQs: the two waveform outputs (One Ramp mode). */
enum {
	AVR_TCD_IRQ_WOA = 0,
	AVR_TCD_IRQ_WOB,
	AVR_TCD_IRQ_COUNT,
};

typedef struct avr_tcd_t {
	avr_io_t	io;
	char		name;

	avr_io_addr_t	base;
	avr_io_addr_t	r_ctrla, r_ctrlb, r_intctrl, r_intflags, r_status;
	avr_io_addr_t	r_faultctrl, r_cmpaset, r_cmpaclr, r_cmpbset, r_cmpbclr;

	avr_int_vector_t	ovf;	/* TCDn_OVF */

	uint32_t		freq_osc20m;	/* unprescaled OSC20M, for CLKSEL=OSC20M */

	avr_cycle_count_t	start_cycle;
	int32_t			start_count;
	uint32_t		prescale;	/* CLK_PER cycles per TCD count */
	uint32_t		top;		/* CMPBCLR captured at start */
	uint8_t			phase;
	int8_t			dir;

	int			base_irq;
	uint8_t		woa, wob;	/* last published output levels */
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

#define AVR_IOCTL_TCD_GETIRQ(_name) AVR_IOCTL_DEF('t','c','d',(_name))

#ifdef __cplusplus
};
#endif

#endif /* __AVR_TCD_H__ */
