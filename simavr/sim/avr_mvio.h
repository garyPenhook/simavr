/*
	avr_mvio.h

	AVR Dx multi-voltage I/O monitor (DS40002413B chapter 19): INTCTRL (VDDIO2IE) at
	+0x00, INTFLAGS (VDDIO2IF, write-1-to-clear) at +0x01, STATUS (VDDIO2S) at
	+0x02. With FUSE.SYSCFG1.MVSYSCFG = SINGLE (2) the status reads 1 and the flag
	never sets; with DUAL (1) VDDIO2S follows the VDDIO2 IRQ, which the board/test
	drives (1 = VDDIO2 in range; it defaults to 1), and a change sets VDDIO2IF.
	The tri-stating of the MVIO pins and the ADC VDDIO2/10 channel are not modelled.

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

#ifndef __AVR_MVIO_H__
#define __AVR_MVIO_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "sim_avr.h"

enum {
	MVIOR_INTCTRL = 0x00,
	MVIOR_INTFLAGS = 0x01,
	MVIOR_STATUS = 0x02,
};

enum {
	AVR_MVIO_IRQ_VDDIO2 = 0,	// drive 1 when VDDIO2 is in range
	AVR_MVIO_IRQ_OK,			// VDDIO2OK event generator level
	AVR_MVIO_IRQ_COUNT,
};

typedef struct avr_mvio_t {
	avr_io_t	io;
	char		name;
	avr_io_addr_t	base;
	avr_int_vector_t	vect;
	uint8_t		syscfg1_fuse;		// index of FUSE.SYSCFG1 in avr->fuse[]
	uint8_t		vddio2_ok;
} avr_mvio_t;

void
avr_mvio_init(
		avr_t * avr,
		avr_mvio_t * p,
		avr_io_addr_t base,
		uint8_t vector,
		uint8_t syscfg1_fuse_index,
		char name);

#define AVR_IOCTL_MVIO_GETIRQ(_name) AVR_IOCTL_DEF('m','v','i',(_name))

#ifdef __cplusplus
};
#endif

#endif /* __AVR_MVIO_H__ */
