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

	Sample accumulation (CTRLB.SAMPNUM) is modelled: a conversion accumulates
	1..64 samples (each taking the per-sample time) and the sum is stored in RES,
	with RESRDY raised only once the whole burst completes. The MUXPOS internal
	channels are addressable too: GND (0x1F) reads 0, and DAC0 (0x1C), the
	internal reference (0x1D) and the temperature sensor (0x1E) are driven as
	settable millivolt inputs like the pin channels (sim_tiny3217 wires DAC0's
	output to its ADC channel).

	The reference is selected by CTRLC.REFSEL: the internal reference (INTREF,
	driven by the VREF peripheral) or VDD / external VREFA (both modelled by the
	settable vref_mv). sim_tiny3217 wires VREF.ADC0REFSEL to the ADC internal
	reference; both references default to 3300 mV until programmed.

	The temperature-sensor channel (MUXPOS 0x1E) returns a calibrated code: given
	a settable die temperature (Kelvin) and the SIGROW.TEMPSENSE0/1 gain/offset,
	it produces the ADC code that inverts the datasheet transfer function
	T_K = ((RES - TEMPSENSE1) * TEMPSENSE0 + 0x80) >> 8, so firmware applying that
	formula recovers the configured temperature. The die temperature is set via
	avr_adc_modern_set_temp_k() or by raising the temperature-sensor channel IRQ
	(MUXPOS 0x1E), which carries Kelvin rather than millivolts.

	Event-triggered start (EVCTRL.STARTEI) is modelled: when enabled, an event
	delivered to the ADC (via avr_adc_modern_event_start, which sim_tiny3217 wires
	to the EVSYS ADC0 user) starts a conversion. The temperature reading is
	independent of the actual REFSEL (the datasheet measurement procedure assumes
	the 1.1V internal reference).

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

#define AVR_ADCM_CHANNELS	32	/* AIN0..11 + internal sources (DAC0/INTREF/TEMP/GND) */
#define AVR_ADCM_CH_DAC0	0x1c
#define AVR_ADCM_CH_INTREF	0x1d
#define AVR_ADCM_CH_TEMPSENSE	0x1e
#define AVR_ADCM_CH_GND		0x1f

/* Register offsets within an ADC block (device header ADC_t). */
enum {
	ADCMR_CTRLA = 0x00,
	ADCMR_CTRLB = 0x01,	/* SAMPNUM accumulation */
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
	avr_io_addr_t	r_ctrla, r_ctrlb, r_ctrlc, r_ctrle, r_muxpos, r_command;
	avr_io_addr_t	r_evctrl, r_intctrl, r_intflags;
	avr_io_addr_t	r_res, r_winlt, r_winht;	/* 16-bit (low byte address) */

	avr_int_vector_t	resrdy;	/* ADCn_RESRDY */
	avr_int_vector_t	wcomp;	/* ADCn_WCOMP (window comparator) */

	uint32_t	vref_mv;	/* VDD / external reference in mV (default 3300) */
	uint32_t	intref_mv;	/* internal reference (CTRLC.REFSEL=INTREF, from VREF) */
	uint16_t	chan_mv[AVR_ADCM_CHANNELS];	/* per-channel input (mV) */
	int			base_irq;	/* global irq number of channel 0 */

	/* Sample accumulation (CTRLB.SAMPNUM) in progress. */
	uint32_t	acc_sum;	/* running sum of samples taken so far */
	uint16_t	acc_count;	/* samples taken in the current burst */
	uint16_t	acc_target;	/* samples to accumulate (1..64) */

	/* Temperature sensor (MUXPOS 0x1E). */
	avr_io_addr_t	r_tempcal;	/* data addr of SIGROW.TEMPSENSE0 (0 = none) */
	int32_t		temp_k;		/* modelled die temperature (Kelvin) */
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

/* Override the modelled VDD / external reference voltage (millivolts). */
void
avr_adc_modern_set_vref(avr_adc_modern_t * p, uint32_t vref_mv);

/* Set the internal reference (CTRLC.REFSEL=INTREF), driven by the VREF block. */
void
avr_adc_modern_set_intref(avr_adc_modern_t * p, uint32_t intref_mv);

/* Point the temperature-sensor channel at the SIGROW.TEMPSENSE0 cal address. */
void
avr_adc_modern_set_tempsense(avr_adc_modern_t * p, avr_io_addr_t tempcal_addr);

/* Set the modelled die temperature (Kelvin) read by the temp-sensor channel. */
void
avr_adc_modern_set_temp_k(avr_adc_modern_t * p, int32_t kelvin);

/* Deliver an event to the ADC: starts a conversion if enabled and EVCTRL.STARTEI
 * is set (used to wire an EVSYS channel to the ADC's event input). */
void
avr_adc_modern_event_start(avr_adc_modern_t * p);

/* Raise AINn (channel 'n') with a millivolt value to drive that analog input. */
#define AVR_IOCTL_ADCM_GETIRQ(_name) AVR_IOCTL_DEF('a','d','m',(_name))

#ifdef __cplusplus
};
#endif

#endif /* __AVR_ADC_MODERN_H__ */
