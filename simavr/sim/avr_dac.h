/*
	avr_dac.h

	"Modern" AVR (AVRxt) 8-bit Digital-to-Analog Converter (DAC0 at 0x06A0 on the
	tinyAVR 1-series, also megaAVR-0 and AVR Dx families).

	Models the converted output voltage: when CTRLA.ENABLE is set, the output is
	(DATA / 256) * VREF millivolts, otherwise 0. The value is published (in
	millivolts) on the OUT IRQ whenever it changes, so it can feed the analog
	comparator (AC's DAC input), the ADC, or be observed by a test/board. The DAC
	has no interrupt.

	Not modelled: the physical output pin buffer (CTRLA.OUTEN — the OUT IRQ is
	always emitted), run-standby, and exact reference selection (the VREF
	peripheral — a plain settable vref_mv is used instead).

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

#ifndef __AVR_DAC_H__
#define __AVR_DAC_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "sim_avr.h"

/* Register offsets within a DAC block (device header DAC_t). */
enum {
	DACR_CTRLA = 0x00,
	DACR_DATA = 0x01,
};

/* IRQ: the converted output voltage (millivolts). */
enum {
	AVR_DAC_IRQ_OUT = 0,
	AVR_DAC_IRQ_COUNT,
};

typedef struct avr_dac_t {
	avr_io_t	io;
	char		name;		/* '0', … */

	avr_io_addr_t	base;
	avr_io_addr_t	r_ctrla, r_data;

	uint32_t	vref_mv;	/* reference voltage in mV (default 1100) */
	uint32_t	out_mv;		/* last published output (mV) */
} avr_dac_t;

/*
 * Initialise a DAC block at data address 'base'. 'name' is a tag for debug and
 * the IRQ ioctl.
 */
void
avr_dac_init(
		avr_t * avr,
		avr_dac_t * p,
		avr_io_addr_t base,
		char name);

/* Override the modelled reference voltage (millivolts). */
void
avr_dac_set_vref(avr_dac_t * p, uint32_t vref_mv);

#define AVR_IOCTL_DAC_GETIRQ(_name) AVR_IOCTL_DEF('d','a','c',(_name))

#ifdef __cplusplus
};
#endif

#endif /* __AVR_DAC_H__ */
