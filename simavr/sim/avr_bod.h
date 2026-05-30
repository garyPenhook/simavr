/*
	avr_bod.h

	"Modern" AVR (AVRxt) Brown-out Detector (BOD at 0x0080 on the tinyAVR
	1-series, also megaAVR-0 and AVR Dx families).

	The BOD monitors the supply voltage (VDD). Its brown-out reset threshold is
	set by CTRLB.LVL (1.8 / 2.6 / 4.2 V) and whether it runs in Active/Idle and
	in sleep is set by CTRLA.ACTIVE / CTRLA.SLEEP. Both CTRLA (except SLEEP) and
	CTRLB are read-only, loaded at reset from FUSE.BODCFG — enabling the BOD is a
	fuse decision, not a runtime one.

	The Voltage Level Monitor (VLM) is an early-warning comparator that fires an
	interrupt before VDD reaches the brown-out level. VLMCTRLA.VLMLVL sets the VLM
	threshold a configurable margin (5 / 15 / 25 %) above the BOD level;
	STATUS.VLMS reads 1 while VDD is below that threshold, and INTFLAGS.VLMIF is
	set (raising the BOD_VLM interrupt if INTCTRL.VLMIE is set) when VDD crosses
	it in the direction selected by INTCTRL.VLMCFG (BELOW / ABOVE / CROSS). VLMS
	and VLMIF are only updated while the BOD is enabled.

	simavr has no physical supply rail, so VDD is a settable voltage (millivolts,
	default 3300) that a board/test drives via avr_bod_set_vdd() or the VDD input
	IRQ; the brown-out *reset* itself is not modelled (only the VLM interrupt
	path).

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

#ifndef __AVR_BOD_H__
#define __AVR_BOD_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "sim_avr.h"

/* Register offsets within a BOD block (device header BOD_t). */
enum {
	BODR_CTRLA = 0x00,
	BODR_CTRLB = 0x01,
	BODR_VLMCTRLA = 0x08,
	BODR_INTCTRL = 0x09,
	BODR_INTFLAGS = 0x0a,
	BODR_STATUS = 0x0b,
};

/* IRQ: a board/test presents the supply voltage (millivolts). */
enum {
	AVR_BOD_IRQ_VDD_IN = 0,
	AVR_BOD_IRQ_COUNT,
};

typedef struct avr_bod_t {
	avr_io_t	io;
	char		name;		/* '0', … */

	avr_io_addr_t	base;
	avr_io_addr_t	r_ctrla, r_ctrlb, r_vlmctrla, r_intctrl, r_intflags, r_status;

	avr_int_vector_t	vlm_vect;	/* BOD_VLM */

	uint8_t		bodcfg_fuse_index;	/* FUSE.BODCFG index (or 0xff for none) */
	uint32_t	vdd_mv;			/* supply voltage in mV (default 3300) */
	uint8_t		prev_below;		/* last "VDD < VLM threshold" (edge detect) */
} avr_bod_t;

/*
 * Initialise a BOD block at data address 'base'. 'vector' is the BOD_VLM
 * interrupt vector, 'bodcfg_fuse_index' the FUSE.BODCFG byte index that CTRLA/B
 * are loaded from at reset (0xff to skip), and 'name' a tag for debug and the
 * IRQ ioctl.
 */
void
avr_bod_init(
		avr_t * avr,
		avr_bod_t * p,
		avr_io_addr_t base,
		uint8_t vector,
		uint8_t bodcfg_fuse_index,
		char name);

/* Override the modelled supply voltage (millivolts). */
void
avr_bod_set_vdd(avr_bod_t * p, uint32_t vdd_mv);

#define AVR_IOCTL_BOD_GETIRQ(_name) AVR_IOCTL_DEF('b','o','d',(_name))

#ifdef __cplusplus
};
#endif

#endif /* __AVR_BOD_H__ */
