/*
	avr_adc_dd.h

	AVR Dx 12-bit differential ADC (DS40002413B chapter 33): CTRLA..CTRLE at
	+0x00..0x04, SAMPCTRL +0x05, MUXPOS +0x08, MUXNEG +0x09, COMMAND +0x0A, EVCTRL
	+0x0B, INTCTRL +0x0C, INTFLAGS +0x0D, DBGCTRL +0x0E, TEMP +0x0F, RES +0x10,
	WINLT +0x12, WINHT +0x14.

	Modelled: single-ended and differential 12/10-bit conversions (Eq. 33-3..6),
	accumulation of 2..128 samples with the Table 33-1/33-2 result formats and
	LEFTADJ, free-running mode, STCONV/SPCONV, event start (EVCTRL.STARTEI), the
	window comparator, RESRDY/WCMP flags that clear on a RES read, and the conversion
	time of 33.3.3.4 (including SAMPDLY, SAMPLEN and INITDLY). The reference comes
	from VREF.ADC0REF (avr_adc_dd_set_ref()); the inputs are millivolts presented on
	the IRQs below (code n = analog input code of MUXPOS/MUXNEG). The temperature
	sensor input carries Kelvin; its result follows the TEMPSENSE0/1 signature-row
	words as in 33.3.3.8, computed for the 2.048 V reference.

	Not modelled: the PRESC-dependent CLK_ADC limits, RUNSTDBY/sleep behaviour,
	DBGCTRL, and the settling delay of a MUX change.

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

#ifndef __AVR_ADC_DD_H__
#define __AVR_ADC_DD_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "sim_avr.h"

enum {
	ADCDD_CTRLA = 0x00,
	ADCDD_CTRLB = 0x01,
	ADCDD_CTRLC = 0x02,
	ADCDD_CTRLD = 0x03,
	ADCDD_CTRLE = 0x04,
	ADCDD_SAMPCTRL = 0x05,
	ADCDD_MUXPOS = 0x08,
	ADCDD_MUXNEG = 0x09,
	ADCDD_COMMAND = 0x0a,
	ADCDD_EVCTRL = 0x0b,
	ADCDD_INTCTRL = 0x0c,
	ADCDD_INTFLAGS = 0x0d,
	ADCDD_DBGCTRL = 0x0e,
	ADCDD_TEMP = 0x0f,
	ADCDD_RES = 0x10,		/* 16-bit */
	ADCDD_WINLT = 0x12,		/* 16-bit */
	ADCDD_WINHT = 0x14,		/* 16-bit */
};

/* MUXPOS / MUXNEG codes beyond AINn (DS40002413B 33.5.7/8). */
#define AVR_ADC_DD_CH_GND			0x40
#define AVR_ADC_DD_CH_TEMPSENSE		0x42
#define AVR_ADC_DD_CH_VDDDIV10		0x44
#define AVR_ADC_DD_CH_VDDIO2DIV10	0x45
#define AVR_ADC_DD_CH_DAC0			0x48
#define AVR_ADC_DD_CH_DACREF0		0x49

enum {
	AVR_ADC_DD_NCHANNELS = 0x4a,	/* IRQ n = analog input code n */
	AVR_ADC_DD_IRQ_EVT_RES = AVR_ADC_DD_NCHANNELS,	/* RESRDY event pulse */
	AVR_ADC_DD_IRQ_VDD,			/* input: VDD (mV), REFSEL=VDD, VDDDIV10 */
	AVR_ADC_DD_IRQ_VDDIO2,		/* input: VDDIO2 (mV), VDDIO2DIV10 */
	AVR_ADC_DD_IRQ_REF,			/* input: the selected reference (mV) */
	AVR_ADC_DD_IRQ_COUNT,
};

typedef struct avr_adc_dd_t {
	avr_io_t	io;
	char		name;
	avr_io_addr_t	base;
	avr_int_vector_t	resrdy, wcmp;	/* ADCn_RESRDY, ADCn_WCMP */
	uint32_t	vdd_mv, vddio2_mv, ref_mv;
	uint16_t	chan_mv[AVR_ADC_DD_NCHANNELS];
	const uint32_t	* dacref_mv;	/* AC0 DACREF output (MUXPOS = DACREF0) */
	avr_io_addr_t	r_tempcal;		/* SIGROW.TEMPSENSE0; +2 = TEMPSENSE1; 0 = none */
	int32_t		temp_k;			/* modelled die temperature (Kelvin) */
	int			base_irq;
	uint8_t		busy;			/* conversion in progress */
	uint8_t		need_init;		/* INITDLY still to elapse (just enabled) */
	int32_t		pending_res;
} avr_adc_dd_t;

void
avr_adc_dd_init(
		avr_t * avr,
		avr_adc_dd_t * p,
		avr_io_addr_t base,
		uint8_t vec_resrdy,
		uint8_t vec_wcmp,
		char name);

/* Selected reference voltage (mV) from VREF.ADC0REF. */
void avr_adc_dd_set_ref(avr_adc_dd_t * p, uint32_t ref_mv);
void avr_adc_dd_set_vdd(avr_adc_dd_t * p, uint32_t vdd_mv);
/* Point the temperature-sensor input at SIGROW.TEMPSENSE0 (TEMPSENSE1 at +2). */
void avr_adc_dd_set_tempsense(avr_adc_dd_t * p, avr_io_addr_t tempcal_addr);
void avr_adc_dd_set_temp_k(avr_adc_dd_t * p, int32_t kelvin);
void avr_adc_dd_set_dacref_source(avr_adc_dd_t * p, const uint32_t * dacref_mv);
/* EVSYS ADC0START user: a rising edge starts a conversion when STARTEI is set. */
void avr_adc_dd_event_start(avr_adc_dd_t * p);

#define AVR_IOCTL_ADC_DD_GETIRQ(_name) AVR_IOCTL_DEF('a','d','d',(_name))

#ifdef __cplusplus
};
#endif

#endif /* __AVR_ADC_DD_H__ */
