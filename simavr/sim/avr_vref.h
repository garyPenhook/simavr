/*
	avr_vref.h

	"Modern" AVR (AVRxt) Voltage Reference (VREF at 0x00A0 on the tinyAVR
	1-series, also megaAVR-0 and AVR Dx families).

	VREF holds the internal-reference selection for the analog peripherals.
	CTRLA selects the reference voltage for ADC0 (ADC0REFSEL[6:4]) and for
	DAC0/AC0 (DAC0REFSEL[2:0]); the options are 0.55 / 1.1 / 2.5 / 4.3 / 1.5 V.
	CTRLB holds per-peripheral "force enable" bits (keep the reference running
	even when not requested) which have no behavioural effect here. CTRLC/CTRLD
	select references for ADC1/DAC1/DAC2, which do not exist on the ATtiny3217
	(the registers are present in the address map and modelled as a store).

	The DAC0/AC0 reference is *always* the internal VREF, so DAC0REFSEL is
	published (decoded to millivolts) on the DAC0 IRQ whenever CTRLA changes;
	sim_tiny3217 wires that to DAC0's and AC0's reference. ADC0's reference is
	selected by ADC.CTRLC.REFSEL (internal VREF vs VDD vs external), which the
	ADC model does not yet distinguish, so ADC0REFSEL is published on its own
	IRQ but left unwired until the ADC models REFSEL — pushing it unconditionally
	would override the ADC's VDD-referenced default.

	The reference selection is published only on a register *write*, never at
	reset, so peripherals keep their own modelled reference defaults until
	firmware actually programs VREF.

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

#ifndef __AVR_VREF_H__
#define __AVR_VREF_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "sim_avr.h"

#define AVR_VREF_REGS	4	/* CTRLA..CTRLD */

/* Register offsets within a VREF block (device header VREF_t). */
enum {
	VREFR_CTRLA = 0x00,
	VREFR_CTRLB = 0x01,
	VREFR_CTRLC = 0x02,
	VREFR_CTRLD = 0x03,
};

/* CTRLA bit fields. */
#define VREF_DAC0REFSEL_gm	0x07	/* DAC0/AC0 reference select [2:0] */
#define VREF_DAC0REFSEL_gp	0
#define VREF_ADC0REFSEL_gm	0x70	/* ADC0 reference select [6:4] */
#define VREF_ADC0REFSEL_gp	4

/* IRQs: the decoded reference voltage (millivolts) for each consumer. */
enum {
	AVR_VREF_IRQ_ADC0_MV = 0,	/* ADC0REFSEL decoded (left unwired, see .h) */
	AVR_VREF_IRQ_DAC0_MV,		/* DAC0REFSEL decoded (feeds DAC0 and AC0) */
	AVR_VREF_IRQ_COUNT,
};

typedef struct avr_vref_t {
	avr_io_t	io;
	char		name;

	avr_io_addr_t	base;
} avr_vref_t;

/*
 * Decode a 3-bit *REFSEL field to its reference voltage in millivolts:
 * 0 -> 550, 1 -> 1100, 2 -> 2500, 3 -> 4300, 4 -> 1500. Reserved values
 * (5..7) return 0.
 */
uint32_t
avr_vref_sel_to_mv(uint8_t sel);

/*
 * Initialise a VREF block at data address 'base'. 'name' is a tag for debug
 * and the IRQ ioctl.
 */
void
avr_vref_init(
		avr_t * avr,
		avr_vref_t * p,
		avr_io_addr_t base,
		char name);

#define AVR_IOCTL_VREF_GETIRQ(_name) AVR_IOCTL_DEF('v','r','f',(_name))

#ifdef __cplusplus
};
#endif

#endif /* __AVR_VREF_H__ */
