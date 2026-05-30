/*
	attiny3217_selftest.c

	A self-checking firmware for the modern-AVR (AVRxt) ATtiny3217 core added by
	this fork. Unlike the hand-assembled opcode checks in test_avrxt_engine.c,
	this is real C compiled with avr-gcc against the device headers, so it also
	validates that the modelled register layout matches what firmware actually
	uses. Each subtest sets a bit in GPIOR0; GPIOR1 is written 0xA5 when done. The
	host harness (test_attiny3217_selftest.c) runs it and checks the result.

	Subtests (all self-contained — no external stimulus needed):
	  bit0  EEPROM write + read-back through NVMCTRL
	  bit1  temperature-sensor channel decodes to ~25 C via the SIGROW cal
	  bit2  TCB0 periodic-interrupt-mode CAPT flag (timer actually counts)
	  bit3  TCA0 overflow flag (timer actually counts)
	  bit4  DAC0 output measured back through the ADC0 internal DAC0 channel
	  bit5  EVSYS software event routed to the ADC0 user starts a conversion

	Build with a modern avr-gcc:
	    avr-gcc -mmcu=attiny3217 -Os -o attiny3217_selftest.axf attiny3217_selftest.c

	Copyright 2026 simavr authors. GNU GPL v3 or later; see COPYING.
 */

#include <avr/io.h>
#include <stdint.h>
#include "avr_mcu_section.h"

AVR_MCU(F_CPU, "attiny3217");

/* Bounded spin so a broken peripheral fails its bit instead of hanging. */
#define WAIT_FLAG(expr, limit) ({			\
	uint32_t _n = (limit); int _ok = 0;		\
	while (_n--) { if (expr) { _ok = 1; break; } }	\
	_ok; })

static uint8_t test_eeprom(void)
{
	uint8_t *ee = (uint8_t *)MAPPED_EEPROM_START;

	if (!WAIT_FLAG(!(NVMCTRL.STATUS & NVMCTRL_EEBUSY_bm), 100000))
		return 0;
	*ee = 0x5a;				/* load the page buffer */
	CCP = CCP_SPM_gc;			/* unlock the protected command */
	NVMCTRL.CTRLA = NVMCTRL_CMD_PAGEERASEWRITE_gc;
	if (!WAIT_FLAG(!(NVMCTRL.STATUS & NVMCTRL_EEBUSY_bm), 100000))
		return 0;
	return *ee == 0x5a;
}

static uint8_t test_tempsense(void)
{
	ADC0.CTRLC = ADC_PRESC_DIV4_gc | ADC_REFSEL_INTREF_gc;
	ADC0.MUXPOS = ADC_MUXPOS_TEMPSENSE_gc;
	ADC0.CTRLA = ADC_ENABLE_bm;			/* 10-bit */
	ADC0.COMMAND = ADC_STCONV_bm;
	if (!WAIT_FLAG(ADC0.INTFLAGS & ADC_RESRDY_bm, 100000))
		return 0;

	/* Datasheet transfer function. */
	int8_t  off  = SIGROW.TEMPSENSE1;
	uint8_t gain = SIGROW.TEMPSENSE0;
	int32_t t = (int32_t)ADC0.RES - off;
	t *= gain;
	t += 0x80;
	t >>= 8;					/* Kelvin */
	return t >= 296 && t <= 300;			/* ~25 C default die temp */
}

static uint8_t test_tcb0(void)
{
	TCB0.CCMP = 200;
	TCB0.CTRLB = TCB_CNTMODE_INT_gc;		/* periodic interrupt mode */
	TCB0.CTRLA = TCB_ENABLE_bm;			/* CLK_PER */
	return WAIT_FLAG(TCB0.INTFLAGS & TCB_CAPT_bm, 100000);
}

static uint8_t test_tca0(void)
{
	TCA0.SINGLE.PER = 300;
	TCA0.SINGLE.CTRLA = TCA_SINGLE_ENABLE_bm;	/* CLK_PER, DIV1 */
	return WAIT_FLAG(TCA0.SINGLE.INTFLAGS & TCA_SINGLE_OVF_bm, 100000);
}

/* DAC0 output, measured back through the ADC0 internal DAC0 channel. With the
 * default DAC reference (~1.1V) and DATA=255 the output is ~1095 mV, which the
 * ADC (VDD = 3.3V reference) reads as ~339 (10-bit). */
static uint8_t test_dac_to_adc(void)
{
	DAC0.DATA = 255;
	DAC0.CTRLA = DAC_ENABLE_bm;

	ADC0.CTRLC = ADC_PRESC_DIV4_gc | ADC_REFSEL_VDDREF_gc;
	ADC0.MUXPOS = ADC_MUXPOS_DAC0_gc;
	ADC0.EVCTRL = 0;
	ADC0.CTRLA = ADC_ENABLE_bm;
	ADC0.INTFLAGS = ADC_RESRDY_bm;
	ADC0.COMMAND = ADC_STCONV_bm;
	if (!WAIT_FLAG(ADC0.INTFLAGS & ADC_RESRDY_bm, 100000))
		return 0;
	uint16_t res = ADC0.RES;
	return res >= 320 && res <= 360;
}

/* A software event on a channel routed to the ADC0 user starts a conversion
 * (ADC EVCTRL.STARTEI armed). Validates the EVSYS user-delivery path end-to-end
 * from real firmware. */
static uint8_t test_evsys_to_adc(void)
{
	ADC0.MUXPOS = ADC_MUXPOS_GND_gc;	/* deterministic result */
	ADC0.CTRLA = ADC_ENABLE_bm;
	ADC0.EVCTRL = ADC_STARTEI_bm;
	ADC0.INTFLAGS = ADC_RESRDY_bm;		/* clear */

	EVSYS.ASYNCUSER1 = 1;			/* ADC0 user <- channel 0 (SYNCCH0) */
	if (ADC0.INTFLAGS & ADC_RESRDY_bm)	/* nothing should have started yet */
		return 0;
	EVSYS.SYNCSTROBE = 0x01;		/* software event on SYNCCH0 */
	return WAIT_FLAG(ADC0.INTFLAGS & ADC_RESRDY_bm, 100000);
}

int
main(void)
{
	uint8_t r = 0;

	if (test_eeprom())	r |= 1 << 0;
	if (test_tempsense())	r |= 1 << 1;
	if (test_tcb0())	r |= 1 << 2;
	if (test_tca0())	r |= 1 << 3;
	if (test_dac_to_adc())	r |= 1 << 4;
	if (test_evsys_to_adc())r |= 1 << 5;

	GPIOR0 = r;
	GPIOR1 = 0xa5;		/* done sentinel for the host harness */

	for (;;)
		;
	return 0;
}
