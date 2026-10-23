/*
	avr_vref.h

	"Modern" AVR (AVRxt) Voltage Reference (VREF at 0x00A0 on the tinyAVR
	1-series, also megaAVR-0 and AVR Dx families).

	VREF holds the internal-reference selection for the analog peripherals
	(register map: DS40002205A 18.4, p.162):
	  CTRLA: ADC0REFSEL[6:4], DAC0REFSEL[2:0]  (DAC0REFSEL feeds DAC0 *and* AC0)
	  CTRLB: per-peripheral "force enable" bits (no behavioural effect here)
	  CTRLC: ADC1REFSEL[6:4], DAC1REFSEL[2:0]  (DAC1REFSEL feeds DAC1 *and* AC1)
	  CTRLD: DAC2REFSEL[2:0]                    (feeds DAC2 *and* AC2)
	The reference options are 0.55 / 1.1 / 2.5 / 4.3 / 1.5 V.

	On the tinyAVR 1-series the 16K/32K parts (attiny1614/1616/1617/3214/3216/
	3217) fit ADC1 and AC1/AC2, so CTRLC/CTRLD are live there. DAC1/DAC2 do not
	exist on any tinyAVR-1 part (there is a single 8-bit DAC0), so DAC1REFSEL and
	DAC2REFSEL only feed AC1 and AC2 respectively. On the smaller tinyAVR-1 parts
	and on megaAVR-0 those consumers are absent, so the published IRQs are simply
	left unwired by the core template.

	The DAC/AC reference is *always* the internal VREF, so each DACnREFSEL is
	published (decoded to millivolts) whenever its CTRL register changes; the
	core wires DAC0_MV to DAC0/AC0, DAC1_MV to AC1, and DAC2_MV to AC2. ADC0/ADC1
	references are selected by ADC.CTRLC.REFSEL (internal VREF vs VDD vs
	external); the ADC model distinguishes the internal reference via
	avr_adc_modern_set_intref(), so ADCnREFSEL is published and the core wires it
	to the matching ADC.

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

/* CTRLC bit fields (same layout as CTRLA, for ADC1 / DAC1+AC1). */
#define VREF_DAC1REFSEL_gm	0x07	/* DAC1/AC1 reference select [2:0] */
#define VREF_DAC1REFSEL_gp	0
#define VREF_ADC1REFSEL_gm	0x70	/* ADC1 reference select [6:4] */
#define VREF_ADC1REFSEL_gp	4

/* CTRLD bit field (for DAC2+AC2). */
#define VREF_DAC2REFSEL_gm	0x07	/* DAC2/AC2 reference select [2:0] */
#define VREF_DAC2REFSEL_gp	0

/* IRQs: the decoded reference voltage (millivolts) for each consumer. */
enum {
	AVR_VREF_IRQ_ADC0_MV = 0,	/* CTRLA.ADC0REFSEL -> ADC0 internal ref */
	AVR_VREF_IRQ_DAC0_MV,		/* CTRLA.DAC0REFSEL -> DAC0 and AC0 */
	AVR_VREF_IRQ_ADC1_MV,		/* CTRLC.ADC1REFSEL -> ADC1 internal ref */
	AVR_VREF_IRQ_DAC1_MV,		/* CTRLC.DAC1REFSEL -> AC1 (DAC1 absent) */
	AVR_VREF_IRQ_DAC2_MV,		/* CTRLD.DAC2REFSEL -> AC2 (DAC2 absent) */
	AVR_VREF_IRQ_COUNT,
	/* tinyAVR 2-series: CTRLA.AC0REFSEL is published on the DAC0 slot (there
	 * is no DAC0; the ADC0 reference is selected inside the ADC itself). */
	AVR_VREF_IRQ_AC0_MV = AVR_VREF_IRQ_DAC0_MV,
	/* AVR Dx: ADC0REF, DAC0REF and ACREF each publish on their own slot. */
	AVR_VREF_IRQ_DD_AC0_MV = AVR_VREF_IRQ_ADC1_MV,
};

typedef struct avr_vref_t {
	avr_io_t	io;
	char		name;

	avr_io_addr_t	base;
	uint8_t		nregs;		/* CTRLx registers fitted (4, or 2 on tinyAVR-2) */
	uint32_t	vdd_mv;		/* REFSEL = VDD (tinyAVR-2 / AVR Dx; default 3300) */
	uint8_t		dd;		/* AVR Dx register set (republish on reset / level change) */
	uint32_t	vrefa_mv;	/* REFSEL = VREFA external reference (AVR Dx; default 0) */
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

/*
 * tinyAVR 2-series VREF (DS40002311B 19.5): CTRLA holds only AC0REFSEL[2:0]
 * and CTRLB the NVM/ADC0/AC0 force-enable bits. Writing CTRLA publishes the
 * decoded AC0 reference (millivolts) on AVR_VREF_IRQ_AC0_MV; AC0REFSEL = VDD
 * publishes the VDD set with avr_vref_set_vdd(). Reserved selections publish 0.
 */
void
avr_vref_init_tiny2(
		avr_t * avr,
		avr_vref_t * p,
		avr_io_addr_t base,
		char name);

/* Set the supply voltage (mV) published when AC0REFSEL selects VDD. */
void
avr_vref_set_vdd(avr_vref_t * p, uint32_t vdd_mv);

#define AVR_IOCTL_VREF_GETIRQ(_name) AVR_IOCTL_DEF('v','r','f',(_name))

/*
 * AVR Dx VREF (DS40002413B 21): ADC0REF at +0x00, DAC0REF at +0x02 and ACREF at
 * +0x04, each ALWAYSON (bit 7) plus REFSEL[2:0]: 1.024 / 2.048 / 4.096 / 2.5 V,
 * VDD (5) or VREFA (6); 4 and 7 are reserved and publish 0. The decoded
 * millivolts are published on AVR_VREF_IRQ_ADC0_MV / _DAC0_MV / _DD_AC0_MV on
 * every write. The VDD and VREFA levels default to 3300 / 0 mV (see
 * avr_vref_set_vdd() and avr_vref_set_vrefa()).
 */
void
avr_vref_init_dd(
		avr_t * avr,
		avr_vref_t * p,
		avr_io_addr_t base,
		char name);

void
avr_vref_set_vrefa(avr_vref_t * p, uint32_t vrefa_mv);

#ifdef __cplusplus
};
#endif

#endif /* __AVR_VREF_H__ */
