/*
	avr_ac_dd.h

	AVR Dx analog comparator (DS40002413B chapter 32). Unlike the tinyAVR/megaAVR
	comparator (avr_ac.h) it has MUXCTRL at +0x02 (INVERT, INITVAL, MUXPOS[2:0],
	MUXNEG[2:0]), DACREF at +0x05, INTMODE in INTCTRL and POWER/HYSMODE/OUTEN in
	CTRLA. On the AVR DD it has two positive inputs (AINP3, AINP4) and two
	negative inputs (AINN2, AINN3) plus the DACREF level.

	Analog voltages (mV) are presented on the AINxx IRQs by the board/test; the
	comparator output is published on the OUT IRQ. DACREF = DACREF * VREF / 256
	with VREF from avr_ac_dd_set_vref() (VREF.ACREF). HYSMODE uses the typical
	VHYST of Table 38-26 (10/25/50 mV); the datasheet does not give the window
	geometry, so VHYST is *assumed* to be the total width (+/- VHYST/2). INITVAL
	and the POWER profile response times are not modelled (a disabled AC reads 0).

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

#ifndef __AVR_AC_DD_H__
#define __AVR_AC_DD_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "sim_avr.h"
#include "avr_ac.h"		// AVR_IOCTL_AC_GETIRQ

enum {
	ACDD_CTRLA = 0x00,
	ACDD_MUXCTRL = 0x02,
	ACDD_DACREF = 0x05,
	ACDD_INTCTRL = 0x06,
	ACDD_STATUS = 0x07,
};

/* IRQs: present mV on the inputs; observe the comparator output on OUT. */
enum {
	AVR_AC_DD_IRQ_AINP3 = 0,
	AVR_AC_DD_IRQ_AINP4,
	AVR_AC_DD_IRQ_AINN2,
	AVR_AC_DD_IRQ_AINN3,
	AVR_AC_DD_IRQ_OUT,
	AVR_AC_DD_IRQ_COUNT,
};

typedef struct avr_ac_dd_t {
	avr_io_t	io;
	char		name;
	avr_io_addr_t	base;
	avr_int_vector_t	vect;		// ACn_AC
	uint16_t	pos_mv[2];			// AINP3, AINP4
	uint16_t	neg_mv[2];			// AINN2, AINN3
	uint32_t	vref_mv;			// VREF.ACREF level
	uint32_t	dacref_mv;			// DACREF output = DACREF * VREF / 256 (read by the ADC)
	uint8_t		prev_state, prev_raw;
	int			base_irq;
} avr_ac_dd_t;

void
avr_ac_dd_init(
		avr_t * avr,
		avr_ac_dd_t * p,
		avr_io_addr_t base,
		uint8_t vector,
		char name);

/* VREF.ACREF level in millivolts (the DACREF divider's full scale). */
void
avr_ac_dd_set_vref(avr_ac_dd_t * p, uint32_t vref_mv);

#ifdef __cplusplus
};
#endif

#endif /* __AVR_AC_DD_H__ */
