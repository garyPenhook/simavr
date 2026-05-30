/*
	avr_adc_modern.h

	Modern AVR ADC (the AVRxt register-block ADC, e.g. ADC0 on tinyAVR 1-series
	at 0x0600). Models single-shot conversions: the analog input for each channel
	is provided (in millivolts) by raising the matching AINn IRQ; writing
	COMMAND.STCONV converts the MUXPOS-selected channel against vref into RES and
	sets the RESRDY flag/interrupt. Free-running mode, the window comparator,
	accumulation and exact reference selection are not modelled.

	Copyright 2024 simavr modern-AVR fork (garyPenhook)

	This file is part of simavr. GNU GPL v3 or later; see COPYING.
 */

#ifndef __AVR_ADC_MODERN_H__
#define __AVR_ADC_MODERN_H__

#include "sim_avr.h"
#include "sim_interrupts.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AVR_ADCM_CHANNELS	16

enum {	// raise AINn with a millivolt value to drive that analog input
	AVR_ADCM_IRQ_AIN0 = 0,
	AVR_ADCM_IRQ_COUNT = AVR_ADCM_CHANNELS,
};

// register offsets from the ADC base
enum {
	AVR_ADCM_CTRLA = 0x00, AVR_ADCM_CTRLC = 0x02, AVR_ADCM_MUXPOS = 0x06,
	AVR_ADCM_COMMAND = 0x08, AVR_ADCM_INTCTRL = 0x0A, AVR_ADCM_INTFLAGS = 0x0B,
	AVR_ADCM_RES = 0x10,
};

#define AVR_ADCM_ENABLE		(1 << 0)	// CTRLA.ENABLE
#define AVR_ADCM_RESSEL		(1 << 2)	// CTRLA.RESSEL (1=8-bit, 0=10-bit)
#define AVR_ADCM_STCONV		(1 << 0)	// COMMAND.STCONV
#define AVR_ADCM_RESRDY		(1 << 0)	// INT*.RESRDY

typedef struct avr_adcm_t {
	avr_io_t			io;
	char				name;		// '0'
	avr_io_addr_t		r_base;
	avr_int_vector_t	resrdy;		// ADCn_RESRDY
	uint32_t			vref_mv;	// reference voltage in mV
	uint16_t			chan_mv[AVR_ADCM_CHANNELS];	// per-channel input (mV)
} avr_adcm_t;

void avr_adcm_init(avr_t * avr, avr_adcm_t * p);

#define AVR_IOCTL_ADCM_GETIRQ(_name) AVR_IOCTL_DEF('a','d','m',(_name))

#ifdef __cplusplus
};
#endif

#endif /* __AVR_ADC_MODERN_H__ */
