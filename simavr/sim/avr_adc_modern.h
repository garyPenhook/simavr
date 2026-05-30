/*
	avr_adc_modern.h

	"Modern" AVR (AVRxt) ADC (the register-block ADC, e.g. ADC0 at 0x0600 on the
	tinyAVR 1-series, also megaAVR-0 and AVR Dx families).

	Models single-shot and free-running conversions. The analog input for each
	channel is presented (in millivolts) by raising the matching AINn IRQ; a
	conversion of the MUXPOS-selected channel against vref produces RES and sets
	RESRDY (plus the window-comparator WCMP flag when armed). Conversions take a
	realistic number of CPU cycles (~13 ADC clocks at the CTRLC prescaler) so
	RESRDY-interrupt and free-running firmware behaves.

	Not modelled: sample accumulation (CTRLB.SAMPNUM — treated as a single
	sample), exact reference selection (CTRLC.REFSEL / the VREF peripheral — a
	plain settable vref_mv is used instead), event-triggered start, the
	temperature sensor / DAC / internal channels.

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

#ifndef __AVR_ADC_MODERN_H__
#define __AVR_ADC_MODERN_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "sim_avr.h"

#define AVR_ADCM_CHANNELS	16

/* Register offsets within an ADC block (device header ADC_t). */
enum {
	ADCMR_CTRLA = 0x00,
	ADCMR_CTRLB = 0x01,
	ADCMR_CTRLC = 0x02,
	ADCMR_CTRLD = 0x03,
	ADCMR_CTRLE = 0x04,
	ADCMR_SAMPCTRL = 0x05,
	ADCMR_MUXPOS = 0x06,
	ADCMR_COMMAND = 0x08,
	ADCMR_EVCTRL = 0x09,
	ADCMR_INTCTRL = 0x0a,
	ADCMR_INTFLAGS = 0x0b,
	ADCMR_TEMP = 0x0d,
	ADCMR_RESL = 0x10,
	ADCMR_RESH = 0x11,
	ADCMR_WINLTL = 0x12,
	ADCMR_WINHTL = 0x14,
};

typedef struct avr_adc_modern_t {
	avr_io_t	io;
	char		name;		/* '0', … */

	avr_io_addr_t	base;
	avr_io_addr_t	r_ctrla, r_ctrlc, r_ctrle, r_muxpos, r_command;
	avr_io_addr_t	r_intctrl, r_intflags;
	avr_io_addr_t	r_res, r_winlt, r_winht;	/* 16-bit (low byte address) */

	avr_int_vector_t	resrdy;	/* ADCn_RESRDY */
	avr_int_vector_t	wcomp;	/* ADCn_WCOMP (window comparator) */

	uint32_t	vref_mv;	/* reference voltage in mV (default 3300) */
	uint16_t	chan_mv[AVR_ADCM_CHANNELS];	/* per-channel input (mV) */
	int			base_irq;	/* global irq number of channel 0 */
} avr_adc_modern_t;

/*
 * Initialise an ADC block at data address 'base'. 'vec_resrdy' is the
 * ADCn_RESRDY vector, 'vec_wcomp' the ADCn_WCOMP (window comparator) vector;
 * 'name' is a tag for debug and the IRQ ioctl.
 */
void
avr_adc_modern_init(
		avr_t * avr,
		avr_adc_modern_t * p,
		avr_io_addr_t base,
		uint8_t vec_resrdy,
		uint8_t vec_wcomp,
		char name);

/* Override the modelled reference voltage (millivolts). */
void
avr_adc_modern_set_vref(avr_adc_modern_t * p, uint32_t vref_mv);

/* Raise AINn (channel 'n') with a millivolt value to drive that analog input. */
#define AVR_IOCTL_ADCM_GETIRQ(_name) AVR_IOCTL_DEF('a','d','m',(_name))

#ifdef __cplusplus
};
#endif

#endif /* __AVR_ADC_MODERN_H__ */
