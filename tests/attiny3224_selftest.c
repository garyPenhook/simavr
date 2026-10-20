/*
	attiny3224_selftest.c

	A self-checking firmware for the tinyAVR 2-series (AVRxt) cores added by this
	fork. Like attiny3217_selftest.c it is real C compiled with avr-gcc against the
	device header, so it validates that the modelled register layout matches what
	firmware actually uses on the 2-series (12-bit differential ADC with PGA,
	AC0 DACREF, channel/user EVSYS, CCL interrupt). Each subtest sets a bit in
	OUT[0], the interrupt subtests set bits in OUT[1], and OUT[2] is
	written 0xA5 when done. The host harness (test_attiny3224_selftest.c) runs it
	and checks the result.

	OUT is a fixed SRAM block rather than GPIOR0-3: it was written when GPIOR0-3
	(0x1C-0x1F) still aliased r28-r31 in the engine (fixed since).

	Every subtest is self-contained (internal inputs only: VDD/10, AC0 DACREF,
	the temperature sensor):
	  OUT[0] bit0  EEPROM write + read-back through NVMCTRL
	  OUT[0] bit1  temperature sensor decodes to ~25 C via the SIGROW cal
	  OUT[0] bit2  VDD/10 reads ~330 mV on the 12-bit ADC
	  OUT[0] bit3  burst accumulation + scaling of the AC0 DACREF input
	  OUT[0] bit4  differential conversion (VDD/10 - DACREF) is negative
	  OUT[0] bit5  window comparator (ABOVE) sets WCMP
	  OUT[0] bit6  EVSYS software event -> ADC START (EVENT_TRIGGER)
	  OUT[0] bit7  CCL LUT0 rising-edge interrupt flag
	  OUT[1] bit0  ADC0_RESRDY interrupt vector taken
	  OUT[1] bit1  CCL_CCL interrupt vector taken
	  OUT[1] bit2  ADC0_ERROR interrupt vector taken (trigger overrun)

	Build with a modern avr-gcc:
	    avr-gcc -mmcu=attiny3224 -Os -o attiny3224_selftest.axf attiny3224_selftest.c

	Copyright 2026 simavr authors. GNU GPL v3 or later; see COPYING.
 */

#include <avr/io.h>
#include <avr/interrupt.h>
#include <stdint.h>
#include "avr_mcu_section.h"

AVR_MCU(F_CPU, "attiny3224");

/* Result block: 256 bytes below RAMEND on every 2-series part (SRAM is
 * 0x3E00..0x3FFF on the 512 B parts), clear of .data/.bss and the stack. */
#define OUT	((volatile uint8_t *)0x3f00)

/* Bounded spin so a broken peripheral fails its bit instead of hanging. */
#define WAIT_FLAG(expr, limit) ({			\
	uint32_t _n = (limit); int _ok = 0;		\
	while (_n--) { if (expr) { _ok = 1; break; } }	\
	_ok; })

static volatile uint8_t isr_seen;

ISR(ADC0_RESRDY_vect)
{
	isr_seen |= 1 << 0;
	ADC0.INTFLAGS = ADC_RESRDY_bm;
}

ISR(CCL_CCL_vect)
{
	isr_seen |= 1 << 1;
	CCL.INTFLAGS = CCL_INT0_bm;
}

ISR(ADC0_ERROR_vect)
{
	isr_seen |= 1 << 2;
	ADC0.INTFLAGS = ADC_TRIGOVR_bm;
}

/* Fresh ADC state: DIV2, a 1 us timebase at the 3.33 MHz reset clock, GND input. */
static void adc_cfg(uint8_t refsel, uint8_t sampdur)
{
	ADC0.CTRLA = 0;
	ADC0.CTRLB = ADC_PRESC_DIV2_gc;
	ADC0.CTRLC = (4 << ADC_TIMEBASE_gp) | refsel;
	ADC0.CTRLD = 0;
	ADC0.CTRLE = sampdur;
	ADC0.CTRLF = 0;
	ADC0.PGACTRL = ADC_ADCPGASAMPDUR_20CLK_gc;
	ADC0.INTCTRL = 0;
	ADC0.INTFLAGS = 0x3f;
	ADC0.MUXPOS = ADC_MUXPOS_GND_gc;
	ADC0.MUXNEG = ADC_MUXNEG_GND_gc;
	ADC0.CTRLA = ADC_ENABLE_bm;
}

static uint8_t adc_run(uint8_t cmd)
{
	ADC0.INTFLAGS = 0x3f;
	ADC0.COMMAND = cmd;
	return WAIT_FLAG(ADC0.INTFLAGS & ADC_RESRDY_bm, 200000);
}

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
	/* DS40002311B 30.3.3.7: 1.024 V reference, >=32 us sample, 12-bit single. */
	adc_cfg(ADC_REFSEL_1024MV_gc, 64);
	ADC0.MUXPOS = ADC_MUXPOS_TEMPSENSE_gc;
	if (!adc_run(ADC_MODE_SINGLE_12BIT_gc | ADC_START_IMMEDIATE_gc))
		return 0;

	int8_t  off  = SIGROW.TEMPSENSE1;
	uint8_t gain = SIGROW.TEMPSENSE0;
	uint16_t adc_reading = ADC0.RESULT >> 2;	/* 10-bit MSb of the result */
	uint32_t t = adc_reading - off;
	t *= gain;
	t += 0x80;
	t >>= 8;					/* Kelvin */
	return t >= 296 && t <= 300;			/* ~25 C default die temp */
}

static uint8_t test_vdd_div10(void)
{
	adc_cfg(ADC_REFSEL_VDD_gc, 2);
	ADC0.MUXPOS = ADC_MUXPOS_VDDDIV10_gc;
	if (!adc_run(ADC_MODE_SINGLE_12BIT_gc | ADC_START_IMMEDIATE_gc))
		return 0;
	uint16_t r = ADC0.RESULT;			/* 330 mV / 3300 mV * 4096 = 409 */
	return r >= 405 && r <= 414;
}

/* AC0 DACREF: 2.5 V * 0x80/256 = 1250 mV, read back through the ADC (VDD
 * reference): 1250/3300*4096 = 1551 per sample. */
static void dacref_setup(void)
{
	VREF.CTRLA = VREF_AC0REFSEL_2V5_gc;
	AC0.DACREF = 0x80;
}

static uint8_t test_burst_dacref(void)
{
	dacref_setup();
	adc_cfg(ADC_REFSEL_VDD_gc, 14);			/* burst needs SAMPDUR >= 12 w/ PGA only */
	ADC0.MUXPOS = ADC_MUXPOS_DACREF0_gc;

	ADC0.CTRLF = ADC_SAMPNUM_ACC16_gc;		/* 16 samples, unscaled */
	if (!adc_run(ADC_MODE_BURST_gc | ADC_START_IMMEDIATE_gc))
		return 0;
	uint32_t acc = ADC0.RESULT;
	uint16_t last = ADC0.SAMPLE;
	if (acc < 16UL * 1550 || acc > 16UL * 1552 || last < 1550 || last > 1552)
		return 0;

	ADC0.CTRLF = ADC_SAMPNUM_ACC64_gc;		/* 64 samples, scaled >> 2 */
	if (!adc_run(ADC_MODE_BURST_SCALING_gc | ADC_START_IMMEDIATE_gc))
		return 0;
	acc = ADC0.RESULT;
	return acc >= 16UL * 1550 && acc <= 16UL * 1552;	/* 64*1551 >> 2 */
}

static uint8_t test_differential(void)
{
	dacref_setup();
	adc_cfg(ADC_REFSEL_VDD_gc, 2);
	ADC0.MUXPOS = ADC_MUXPOS_VDDDIV10_gc;		/* 330 mV */
	ADC0.MUXNEG = ADC_MUXNEG_DACREF0_gc;		/* 1250 mV */
	if (!adc_run(ADC_DIFF_bm | ADC_MODE_SINGLE_12BIT_gc | ADC_START_IMMEDIATE_gc))
		return 0;
	int32_t r = (int32_t)ADC0.RESULT;		/* (330-1250)/3300*2048 = -571 */
	int16_t s = (int16_t)ADC0.SAMPLE;
	return r >= -575 && r <= -567 && r == s;
}

static uint8_t test_window(void)
{
	dacref_setup();
	adc_cfg(ADC_REFSEL_VDD_gc, 2);
	ADC0.MUXPOS = ADC_MUXPOS_DACREF0_gc;		/* 1551 */
	ADC0.WINHT = 1000;
	ADC0.CTRLD = ADC_WINCM_ABOVE_gc;
	if (!adc_run(ADC_MODE_SINGLE_12BIT_gc | ADC_START_IMMEDIATE_gc))
		return 0;
	if (!(ADC0.INTFLAGS & ADC_WCMP_bm))
		return 0;				/* 1551 > 1000 must match */
	ADC0.WINHT = 2000;
	if (!adc_run(ADC_MODE_SINGLE_12BIT_gc | ADC_START_IMMEDIATE_gc))
		return 0;
	return !(ADC0.INTFLAGS & ADC_WCMP_bm);		/* 1551 > 2000 must not */
}

/* A software event on channel 0, routed to the ADC START user, starts a
 * conversion (COMMAND.START = EVENT_TRIGGER). */
static uint8_t test_evsys_to_adc(void)
{
	adc_cfg(ADC_REFSEL_VDD_gc, 2);
	ADC0.MUXPOS = ADC_MUXPOS_GND_gc;
	EVSYS.USERADC0START = EVSYS_USER_CHANNEL0_gc;
	ADC0.COMMAND = ADC_MODE_SINGLE_12BIT_gc | ADC_START_EVENT_TRIGGER_gc;
	if (WAIT_FLAG(ADC0.INTFLAGS & ADC_RESRDY_bm, 2000))
		return 0;				/* nothing should have started yet */
	EVSYS.SWEVENTA = 0x01;			/* software event on CHANNEL0 */
	uint8_t ok = WAIT_FLAG(ADC0.INTFLAGS & ADC_RESRDY_bm, 200000);
	ADC0.COMMAND = ADC_START_STOP_gc;
	return ok;
}

/* LUT0 with every input masked and TRUTH0 = 0x01 outputs 1 as soon as it is
 * enabled: a rising edge, which INTMODE0 = RISING flags in INTFLAGS.INT0 (after
 * the LUT's propagation delay, so poll for it). */
static uint8_t test_ccl_flag(void)
{
	CCL.CTRLA = 0;
	CCL.LUT0CTRLA = 0;
	CCL.LUT0CTRLB = CCL_INSEL0_MASK_gc | CCL_INSEL1_MASK_gc;
	CCL.LUT0CTRLC = CCL_INSEL2_MASK_gc;
	CCL.TRUTH0 = 0x01;
	CCL.INTCTRL0 = CCL_INTMODE0_RISING_gc;
	CCL.INTFLAGS = CCL_INT0_bm;
	CCL.LUT0CTRLA = CCL_ENABLE_bm;
	CCL.CTRLA = CCL_ENABLE_bm;
	uint8_t ok = WAIT_FLAG(CCL.INTFLAGS & CCL_INT0_bm, 1000);
	CCL.INTFLAGS = CCL_INT0_bm;
	CCL.INTCTRL0 = 0;
	CCL.CTRLA = 0;
	CCL.LUT0CTRLA = 0;
	return ok;
}

/* ADC0_RESRDY and CCL_CCL vectors reach their handlers; a second trigger while
 * converting raises ADC0_ERROR. */
static uint8_t test_isr(void)
{
	uint8_t r = 0;

	adc_cfg(ADC_REFSEL_VDD_gc, 2);
	ADC0.INTCTRL = ADC_RESRDY_bm;
	sei();
	ADC0.COMMAND = ADC_MODE_SINGLE_12BIT_gc | ADC_START_IMMEDIATE_gc;
	if (WAIT_FLAG(isr_seen & (1 << 0), 200000))
		r |= 1 << 0;

	adc_cfg(ADC_REFSEL_VDD_gc, 2);
	ADC0.INTCTRL = ADC_TRIGOVR_bm;
	ADC0.COMMAND = ADC_MODE_SINGLE_12BIT_gc | ADC_START_IMMEDIATE_gc;
	ADC0.COMMAND = ADC_MODE_SINGLE_12BIT_gc | ADC_START_IMMEDIATE_gc;
	if (WAIT_FLAG(isr_seen & (1 << 2), 200000))
		r |= 1 << 2;
	ADC0.INTCTRL = 0;

	CCL.CTRLA = 0;
	CCL.LUT0CTRLA = 0;
	CCL.LUT0CTRLB = CCL_INSEL0_MASK_gc | CCL_INSEL1_MASK_gc;
	CCL.LUT0CTRLC = CCL_INSEL2_MASK_gc;
	CCL.TRUTH0 = 0x01;
	CCL.INTCTRL0 = CCL_INTMODE0_RISING_gc;
	CCL.INTFLAGS = CCL_INT0_bm;
	CCL.LUT0CTRLA = CCL_ENABLE_bm;
	CCL.CTRLA = CCL_ENABLE_bm;
	if (WAIT_FLAG(isr_seen & (1 << 1), 200000))
		r |= 1 << 1;
	cli();
	return r;
}

int
main(void)
{
	uint8_t r = 0;

	if (test_eeprom())	r |= 1 << 0;
	if (test_tempsense())	r |= 1 << 1;
	if (test_vdd_div10())	r |= 1 << 2;
	if (test_burst_dacref())r |= 1 << 3;
	if (test_differential())r |= 1 << 4;
	if (test_window())	r |= 1 << 5;
	if (test_evsys_to_adc())r |= 1 << 6;
	if (test_ccl_flag())	r |= 1 << 7;

	OUT[0] = r;
	OUT[1] = test_isr();
	OUT[2] = 0xa5;	/* done sentinel for the host harness */

	for (;;)
		;
	return 0;
}
