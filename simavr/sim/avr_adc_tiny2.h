/*
	avr_adc_tiny2.h

	tinyAVR(R) 2-series 12-bit differential ADC with PGA (AVRxt). Register map,
	modes and timing verified against ATtiny424/426/427/824/826/827 DS40002311B
	chapter 30 (the 1624/1626/1627 and 3224/3226/3227 datasheets describe the
	same block).

	Not related to the 10-bit ADC in avr_adc_modern.[ch] (tinyAVR-0/1,
	megaAVR-0), whose register layout it does not share.

	Modelled: Single 8/12-bit, Series and Burst accumulation (with and without
	scaling), single-ended and differential conversions, the PGA (gain 1x-16x),
	FREERUN, the three START triggers (immediate / MUXPOS write / MUXNEG write)
	plus the EVSYS event trigger, LEFTADJ result formats, the window comparator,
	the six interrupt flags on the ERROR / SAMPRDY / RESRDY vectors, and
	datasheet conversion timing including the analog-module initialisation time
	(removed by LOWLAT once the modules are warm). The ADC clock is CLK_PER
	divided by CTRLB.PRESC, and CLK_PER is taken to be the CPU clock (as in
	avr_adc_modern). Inputs are supplied in millivolts on the AINn IRQs; the
	temperature-sensor IRQ carries Kelvin instead. TIMEBASE, RUNSTDBY, DBGRUN
	and PGABIASSEL are stored but have no effect (no power/clock model).

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

#ifndef __AVR_ADC_TINY2_H__
#define __AVR_ADC_TINY2_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "sim_avr.h"

/* Register offsets within the ADC block (DS40002311B 30.4). */
enum {
	ADC2R_CTRLA = 0x00,
	ADC2R_CTRLB = 0x01,
	ADC2R_CTRLC = 0x02,
	ADC2R_CTRLD = 0x03,
	ADC2R_INTCTRL = 0x04,
	ADC2R_INTFLAGS = 0x05,
	ADC2R_STATUS = 0x06,
	ADC2R_DBGCTRL = 0x07,
	ADC2R_CTRLE = 0x08,
	ADC2R_CTRLF = 0x09,
	ADC2R_COMMAND = 0x0a,
	ADC2R_PGACTRL = 0x0b,
	ADC2R_MUXPOS = 0x0c,
	ADC2R_MUXNEG = 0x0d,
	ADC2R_RESULT = 0x10,	/* 32-bit, RESULT0..3 */
	ADC2R_SAMPLE = 0x14,	/* 16-bit, SAMPLEL/H */
	ADC2R_TEMP0 = 0x18,
	ADC2R_WINLT = 0x1c,	/* 16-bit */
	ADC2R_WINHT = 0x1e,	/* 16-bit */
};

/* MUXPOS/MUXNEG input codes beyond AINn (DS40002311B 30.5.13-14). */
#define AVR_ADC2_CH_GND		0x30
#define AVR_ADC2_CH_VDDDIV10	0x31
#define AVR_ADC2_CH_TEMPSENSE	0x32
#define AVR_ADC2_CH_DACREF0	0x33

/*
 * IRQs. Index n (0..0x33) is analog input code n: raise it with a millivolt
 * value to drive AINn (the temperature-sensor input carries Kelvin; GND,
 * VDDDIV10 and DACREF0 are internal and ignore writes). The EVT_* IRQs emit a
 * one-cycle pulse for the RES / SAMP / WCMP event generators (1 then 0). The
 * VDD and VREFA IRQs set the supply and external-reference voltages (mV).
 */
enum {
	AVR_ADC2_NCHANNELS = 0x34,
	AVR_ADC2_IRQ_EVT_RES = AVR_ADC2_NCHANNELS,
	AVR_ADC2_IRQ_EVT_SAMP,
	AVR_ADC2_IRQ_EVT_WCMP,
	AVR_ADC2_IRQ_VDD,	/* input: supply voltage (mV); REFSEL=VDD, VDDDIV10 */
	AVR_ADC2_IRQ_VREFA,	/* input: external VREFA pin voltage (mV) */
	AVR_ADC2_IRQ_COUNT,
};

typedef struct avr_adc_tiny2_t {
	avr_io_t	io;
	char		name;		/* '0', … */

	avr_io_addr_t	base;
	avr_int_vector_t	error;		/* ADCn_ERROR: TRIGOVR/SAMPOVR/RESOVR */
	avr_int_vector_t	samprdy;	/* ADCn_SAMPRDY (+ WCMP when WINSRC=SAMPLE) */
	avr_int_vector_t	resrdy;		/* ADCn_RESRDY (+ WCMP when WINSRC=RESULT) */

	uint32_t	vdd_mv;		/* VDD (default 3300), REFSEL=VDD, VDDDIV10 */
	uint32_t	vrefa_mv;	/* external VREFA (default 3300) */
	uint16_t	chan_mv[AVR_ADC2_NCHANNELS];	/* per-input voltage (mV) */
	const uint32_t	* dacref_mv;	/* AC0 DACREF output (MUXPOS/NEG = DACREF0) */
	avr_io_addr_t	r_tempcal;	/* SIGROW.TEMPSENSE0 (gain); +1 = offset; 0 = none */
	int32_t		temp_k;		/* modelled die temperature (Kelvin) */
	int			base_irq;

	/* Conversion engine state. */
	uint8_t		busy;		/* initialising or converting */
	uint8_t		freerun_active;	/* free-running restart armed */
	uint8_t		warm;		/* analog modules left powered by LOWLAT */
	uint16_t	acc_count;	/* samples accumulated in the current sequence */
	uint16_t	burst_done;	/* burst samples completed so far */
	int64_t		acc_sum;	/* accumulator */
} avr_adc_tiny2_t;

/*
 * Initialise an ADC block at data address 'base'. 'vec_error', 'vec_resrdy'
 * and 'vec_samprdy' are the ADCn_ERROR / RESRDY / SAMPRDY vectors.
 */
void
avr_adc_tiny2_init(
		avr_t * avr,
		avr_adc_tiny2_t * p,
		avr_io_addr_t base,
		uint8_t vec_error,
		uint8_t vec_resrdy,
		uint8_t vec_samprdy,
		char name);

/* Supply / external reference voltages (millivolts). */
void
avr_adc_tiny2_set_vdd(avr_adc_tiny2_t * p, uint32_t vdd_mv);
void
avr_adc_tiny2_set_vrefa(avr_adc_tiny2_t * p, uint32_t vrefa_mv);

/* Point the temperature-sensor input at SIGROW.TEMPSENSE0 (gain; offset follows). */
void
avr_adc_tiny2_set_tempsense(avr_adc_tiny2_t * p, avr_io_addr_t tempcal_addr);

/* Modelled die temperature (Kelvin) seen by the temperature-sensor input. */
void
avr_adc_tiny2_set_temp_k(avr_adc_tiny2_t * p, int32_t kelvin);

/* Track the AC0 DACREF output (mV) for the DACREF0 input (pointer stays live). */
void
avr_adc_tiny2_set_dacref_source(avr_adc_tiny2_t * p, const uint32_t * dacref_mv);

/* Deliver an EVSYS event (rising edge) to the ADC START user: starts a
 * conversion if COMMAND.START = EVENT_TRIGGER. */
void
avr_adc_tiny2_event_start(avr_adc_tiny2_t * p);

#define AVR_IOCTL_ADC2_GETIRQ(_name) AVR_IOCTL_DEF('a','d','2',(_name))

#ifdef __cplusplus
};
#endif

#endif /* __AVR_ADC_TINY2_H__ */
