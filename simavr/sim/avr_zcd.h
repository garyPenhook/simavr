/*
	avr_zcd.h

	AVR Dx zero-cross detector (DS40002413B chapter 35): CTRLA at +0x00 (RUNSTDBY,
	OUTEN, INVERT, ENABLE), INTCTRL at +0x02 (INTMODE[1:0]), STATUS at +0x03
	(STATE, CROSSIF).

	The board/test presents the ZCIN pin voltage (mV) on the ZCIN IRQ; the
	detector output (STATE, optionally inverted) is published on OUT. The switching
	level is the typical zero-cross reference voltage ZCPINV = 0.95 V (Table
	38-27). The synchronizer delay, the current source/sink on the pin and the
	response times are not modelled.

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

#ifndef __AVR_ZCD_H__
#define __AVR_ZCD_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "sim_avr.h"

enum {
	ZCDR_CTRLA = 0x00,
	ZCDR_INTCTRL = 0x02,
	ZCDR_STATUS = 0x03,
};

enum {
	AVR_ZCD_IRQ_ZCIN = 0,	// present the pin voltage in millivolts
	AVR_ZCD_IRQ_OUT,		// detector output level
	AVR_ZCD_IRQ_COUNT,
};

#define AVR_ZCD_ZCPINV_MV	950

typedef struct avr_zcd_t {
	avr_io_t	io;
	char		name;
	avr_io_addr_t	base;
	avr_int_vector_t	vect;		// ZCDn_ZCD
	uint32_t	zcin_mv;
	uint8_t		prev_state;
} avr_zcd_t;

void
avr_zcd_init(
		avr_t * avr,
		avr_zcd_t * p,
		avr_io_addr_t base,
		uint8_t vector,
		char name);

#define AVR_IOCTL_ZCD_GETIRQ(_name) AVR_IOCTL_DEF('z','c','d',(_name))

#ifdef __cplusplus
};
#endif

#endif /* __AVR_ZCD_H__ */
