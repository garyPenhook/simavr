/*
	avr_ac.h

	"Modern" AVR (AVRxt) Analog Comparator (AC0 at 0x0680 on the tinyAVR
	1-series, also megaAVR-0 and AVR Dx families).

	Models the comparator as a combinational function of its selected positive
	and negative inputs: STATUS.STATE = (V+ > V-), optionally inverted. Analog
	input voltages (millivolts) are presented by raising the matching AINPn /
	AINNn IRQ; the VREF and DAC negative references are settable values. On the
	edge selected by CTRLA.INTMODE the STATUS.CMP flag is set and the AC
	interrupt raised (if enabled), and the output is mirrored on an OUT IRQ.

	Not modelled: hysteresis (CTRLA.HYSMODE), low-power / run-standby timing, the
	physical output pin buffer (CTRLA.OUTEN — the OUT IRQ is always emitted).

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

#ifndef __AVR_AC_H__
#define __AVR_AC_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "sim_avr.h"

/* Register offsets within an AC block (device header AC_t). */
enum {
	ACR_CTRLA = 0x00,
	ACR_MUXCTRLA = 0x02,
	ACR_INTCTRL = 0x06,
	ACR_STATUS = 0x07,
};

/* IRQs: present analog voltages (mV) on the positive/negative pin inputs;
 * observe the comparator output (0/1) on OUT. */
enum {
	AVR_AC_IRQ_AINP0 = 0,
	AVR_AC_IRQ_AINP1,
	AVR_AC_IRQ_AINP2,
	AVR_AC_IRQ_AINP3,
	AVR_AC_IRQ_AINN0,
	AVR_AC_IRQ_AINN1,
	AVR_AC_IRQ_OUT,
	AVR_AC_IRQ_COUNT,
};

#define AVR_AC_POS_PINS	4
#define AVR_AC_NEG_PINS	2

typedef struct avr_ac_t {
	avr_io_t	io;
	char		name;		/* '0', … */

	avr_io_addr_t	base;
	avr_io_addr_t	r_ctrla, r_muxctrla, r_intctrl, r_status;

	avr_int_vector_t	vect;	/* ACn_AC */

	uint16_t	pos_mv[AVR_AC_POS_PINS];	/* AINP0..3 (mV) */
	uint16_t	neg_mv[AVR_AC_NEG_PINS];	/* AINN0..1 (mV) */
	uint32_t	vref_mv;	/* internal reference (MUXNEG = VREF) */
	uint32_t	dacref_mv;	/* DAC output (MUXNEG = DAC) */
	uint8_t		prev_state;	/* last comparator output (for edge detect) */
	int			base_irq;	/* global irq number of AINP0 */
} avr_ac_t;

/*
 * Initialise an AC block at data address 'base'. 'vector' is the ACn_AC
 * interrupt vector; 'name' is a tag for debug and the IRQ ioctl.
 */
void
avr_ac_init(
		avr_t * avr,
		avr_ac_t * p,
		avr_io_addr_t base,
		uint8_t vector,
		char name);

/* Override the modelled VREF / DAC negative-reference voltages (millivolts). */
void
avr_ac_set_refs(avr_ac_t * p, uint32_t vref_mv, uint32_t dacref_mv);

#define AVR_IOCTL_AC_GETIRQ(_name) AVR_IOCTL_DEF('a','c',' ',(_name))

#ifdef __cplusplus
};
#endif

#endif /* __AVR_AC_H__ */
