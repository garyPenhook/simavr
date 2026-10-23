/*
	avr_adc_dd.c

	AVR Dx 12-bit differential ADC. See avr_adc_dd.h.

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

#include <stdio.h>
#include <string.h>
#include "avr_adc_dd.h"
#include "sim_cycle_timers.h"

/* CTRLA */
#define ENABLE_bm		0x01
#define FREERUN_bm		0x02
#define RESSEL_10BIT	0x04
#define LEFTADJ_bm		0x10
#define CONVMODE_bm		0x20
/* CTRLB / CTRLC / CTRLD / CTRLE */
#define SAMPNUM_gm		0x07
#define PRESC_gm		0x0f
#define SAMPDLY_gm		0x0f
#define INITDLY_gm		0xe0
#define INITDLY_gp		5
#define WINCM_gm		0x07
/* COMMAND */
#define STCONV_bm		0x01
#define SPCONV_bm		0x02
/* EVCTRL / INTCTRL / INTFLAGS */
#define STARTEI_bm		0x01
#define RESRDY_bm		0x01
#define WCMP_bm			0x02

/* CTRLC.PRESC -> CLK_PER / n (DS40002413B 33.5.3). */
static const uint16_t presc_div[16] = {
	2, 4, 8, 12, 16, 20, 24, 28, 32, 48, 64, 96, 128, 256, 256, 256,
};
/* CTRLD.INITDLY -> CLK_ADC cycles (0, 16, 32, 64, 128, 256). */
static const uint16_t initdly_cycles[8] = { 0, 16, 32, 64, 128, 256, 256, 256 };

static inline uint8_t
reg(avr_adc_dd_t * p, int off)
{
	return p->io.avr->data[p->base + off];
}

static inline void
set(avr_adc_dd_t * p, int off, uint8_t v)
{
	avr_core_watch_write(p->io.avr, p->base + off, v);
}

static inline uint16_t
reg16(avr_adc_dd_t * p, int off)
{
	return reg(p, off) | (reg(p, off + 1) << 8);
}

/* Voltage (mV) on the input selected by 'code'. */
static int32_t
adc_input_mv(avr_adc_dd_t * p, uint8_t code)
{
	switch (code) {
	case AVR_ADC_DD_CH_GND:
		return 0;
	case AVR_ADC_DD_CH_VDDDIV10:
		return p->vdd_mv / 10;
	case AVR_ADC_DD_CH_VDDIO2DIV10:
		return p->vddio2_mv / 10;
	case AVR_ADC_DD_CH_DACREF0:
		return p->dacref_mv ? (int32_t)*p->dacref_mv : 0;
	case AVR_ADC_DD_CH_TEMPSENSE: {
		/* 33.3.3.8: T = (Offset - RES) * Slope / 4096 for the 2.048 V reference,
		 * so RES = Offset - T * 4096 / Slope; V = RES / 4096 * 2.048 V. */
		if (!p->r_tempcal)
			return 0;
		avr_t * avr = p->io.avr;
		int32_t slope = avr->data[p->r_tempcal] | (avr->data[p->r_tempcal + 1] << 8);
		int32_t offset = avr->data[p->r_tempcal + 2] | (avr->data[p->r_tempcal + 3] << 8);
		if (!slope)
			return 0;
		int32_t res = offset - (int32_t)(((int64_t)p->temp_k * 4096) / slope);
		return (int32_t)(((int64_t)res * 2048) / 4096);
	}
	default:
		return code < AVR_ADC_DD_NCHANNELS ? p->chan_mv[code] : 0;
	}
}

static int32_t
clamp(int64_t v, int32_t lo, int32_t hi)
{
	return v < lo ? lo : v > hi ? hi : (int32_t)v;
}

/* One conversion of the current inputs (Equations 33-3 to 33-6). */
static int32_t
adc_sample(avr_adc_dd_t * p, int bits10)
{
	uint8_t ctrla = reg(p, ADCDD_CTRLA);
	int64_t ref = p->ref_mv ? p->ref_mv : 1;
	int64_t vp = adc_input_mv(p, reg(p, ADCDD_MUXPOS) & 0x7f);

	if (ctrla & CONVMODE_bm) {
		int64_t vn = adc_input_mv(p, reg(p, ADCDD_MUXNEG) & 0x7f);
		int32_t full = bits10 ? 512 : 2048;
		/* floor() so negative values round toward -inf like a truncating ADC */
		int64_t num = (vp - vn) * full;
		int64_t q = num / ref;
		if (num % ref < 0)
			q--;
		return clamp(q, -full, full - 1);
	}
	return clamp(vp * (bits10 ? 1024 : 4096) / ref, 0, bits10 ? 1023 : 4095);
}

/* Final RES register value for the current configuration (Tables 33-1/33-2). */
static int32_t
adc_result(avr_adc_dd_t * p)
{
	uint8_t ctrla = reg(p, ADCDD_CTRLA);
	int n = 1 << (reg(p, ADCDD_CTRLB) & SAMPNUM_gm);
	/* Accumulated conversions are always 12-bit (33.3.3.1). */
	int bits10 = (ctrla & RESSEL_10BIT) && n == 1;
	int32_t one = adc_sample(p, bits10);
	int64_t acc = (int64_t)one * n;
	int shift = 0;
	int width = bits10 ? 10 : 12;

	if (n > 16) {
		/* 32/64/128 samples: truncate the LSbs to fit 16 bits. */
		acc >>= (n == 32) ? 1 : (n == 64) ? 2 : 3;
	} else if (ctrla & LEFTADJ_bm) {
		/* 8 MSbs into the high byte: 12-bit conversion << 4, accumulation of n
		 * samples << (4 - log2 n); a 10-bit conversion is placed the same way. */
		int log2n = n == 1 ? 0 : n == 2 ? 1 : n == 4 ? 2 : n == 8 ? 3 : 4;
		shift = 16 - (width + log2n);
	}
	return (int32_t)(acc * (1 << shift));
}

static void
adc_stop_busy(avr_adc_dd_t * p)
{
	p->busy = 0;
	set(p, ADCDD_COMMAND, reg(p, ADCDD_COMMAND) & ~STCONV_bm);
}

static avr_cycle_count_t adc_done(struct avr_t * avr, avr_cycle_count_t when, void * param);

/* Start a conversion: schedule completion after the 33.3.3.4 conversion time. */
static void
adc_start(avr_adc_dd_t * p)
{
	avr_t * avr = p->io.avr;
	uint8_t ctrla = reg(p, ADCDD_CTRLA);
	int n = 1 << (reg(p, ADCDD_CTRLB) & SAMPNUM_gm);
	int bits10 = (ctrla & RESSEL_10BIT) && n == 1;
	uint32_t div = presc_div[reg(p, ADCDD_CTRLC) & PRESC_gm];
	/* Twice the CLK_ADC cycles of one sample: 2 + SAMPDLY + SAMPLEN sampling and
	 * 13.5 (12-bit) or 11.5 (10-bit) converting. */
	uint32_t half = (bits10 ? 23 : 27) +
			2 * (2 + (reg(p, ADCDD_CTRLD) & SAMPDLY_gm) + reg(p, ADCDD_SAMPCTRL));
	uint32_t init = p->need_init ?
			initdly_cycles[(reg(p, ADCDD_CTRLD) & INITDLY_gm) >> INITDLY_gp] : 0;
	/* 2 CLK_PER start-up, the samples, 2 CLK_PER result formatting. */
	uint64_t cyc = 4 + (((uint64_t)n * half + 2 * init) * div) / 2;

	p->need_init = 0;
	p->busy = 1;
	set(p, ADCDD_COMMAND, reg(p, ADCDD_COMMAND) | STCONV_bm);
	avr_cycle_timer_register(avr, cyc ? cyc : 1, adc_done, p);
}

static avr_cycle_count_t
adc_done(struct avr_t * avr, avr_cycle_count_t when, void * param)
{
	avr_adc_dd_t * p = (avr_adc_dd_t *)param;
	uint8_t ctrla = reg(p, ADCDD_CTRLA);
	int32_t res = adc_result(p);
	uint8_t flags = reg(p, ADCDD_INTFLAGS) | RESRDY_bm;
	(void)when;

	set(p, ADCDD_RES, res & 0xff);
	set(p, ADCDD_RES + 1, (res >> 8) & 0xff);

	/* Window comparator (33.5.5): signed compare in differential mode. */
	int32_t value = (ctrla & CONVMODE_bm) ? (int16_t)res : (uint16_t)res;
	int32_t lt = (ctrla & CONVMODE_bm) ? (int16_t)reg16(p, ADCDD_WINLT) : reg16(p, ADCDD_WINLT);
	int32_t ht = (ctrla & CONVMODE_bm) ? (int16_t)reg16(p, ADCDD_WINHT) : reg16(p, ADCDD_WINHT);
	int hit = 0;
	switch (reg(p, ADCDD_CTRLE) & WINCM_gm) {
	case 1: hit = value < lt; break;
	case 2: hit = value > ht; break;
	case 3: hit = value >= lt && value <= ht; break;
	case 4: hit = value < lt || value > ht; break;
	}
	if (hit)
		flags |= WCMP_bm;
	set(p, ADCDD_INTFLAGS, flags);

	avr_raise_irq(p->io.irq + AVR_ADC_DD_IRQ_EVT_RES, 1);
	avr_raise_irq(p->io.irq + AVR_ADC_DD_IRQ_EVT_RES, 0);
	if (reg(p, ADCDD_INTCTRL) & RESRDY_bm)
		avr_raise_interrupt(avr, &p->resrdy);
	if (hit && (reg(p, ADCDD_INTCTRL) & WCMP_bm))
		avr_raise_interrupt(avr, &p->wcmp);

	/* Free-running: the next conversion follows immediately, STCONV stays set. */
	if ((ctrla & FREERUN_bm) && (reg(p, ADCDD_CTRLA) & ENABLE_bm)) {
		adc_start(p);
		return 0;
	}
	adc_stop_busy(p);
	return 0;
}

static void
adc_ctrla_write(struct avr_t * avr, avr_io_addr_t addr, uint8_t v, void * param)
{
	avr_adc_dd_t * p = (avr_adc_dd_t *)param;
	uint8_t old = reg(p, ADCDD_CTRLA);

	avr_core_watch_write(avr, addr, v & 0xbf);
	if ((v & ENABLE_bm) && !(old & ENABLE_bm))
		p->need_init = 1;
	if (!(v & ENABLE_bm) && (old & ENABLE_bm)) {
		avr_cycle_timer_cancel(avr, adc_done, p);
		adc_stop_busy(p);
	}
}

static void
adc_command_write(struct avr_t * avr, avr_io_addr_t addr, uint8_t v, void * param)
{
	avr_adc_dd_t * p = (avr_adc_dd_t *)param;

	/* SPCONV takes precedence over STCONV (33.5.9); both read back through STCONV. */
	if (v & SPCONV_bm) {
		avr_cycle_timer_cancel(avr, adc_done, p);
		adc_stop_busy(p);
		return;
	}
	if ((v & STCONV_bm) && (reg(p, ADCDD_CTRLA) & ENABLE_bm) && !p->busy)
		adc_start(p);
}

/* Writing 1 clears a flag; reading RES clears both (33.5.12). */
static void
adc_intflags_write(struct avr_t * avr, avr_io_addr_t addr, uint8_t v, void * param)
{
	avr_adc_dd_t * p = (avr_adc_dd_t *)param;
	uint8_t res = avr->data[addr] & ~(v & (RESRDY_bm | WCMP_bm));

	avr_core_watch_write(avr, addr, res);
	if (!(res & RESRDY_bm))
		avr_clear_interrupt(avr, &p->resrdy);
	if (!(res & WCMP_bm))
		avr_clear_interrupt(avr, &p->wcmp);
}

static uint8_t
adc_res_read(struct avr_t * avr, avr_io_addr_t addr, void * param)
{
	avr_adc_dd_t * p = (avr_adc_dd_t *)param;
	uint8_t v = avr->data[addr];

	avr_core_watch_write(avr, p->base + ADCDD_INTFLAGS, avr->data[p->base + ADCDD_INTFLAGS] & ~(RESRDY_bm | WCMP_bm));
	avr_clear_interrupt(avr, &p->resrdy);
	avr_clear_interrupt(avr, &p->wcmp);
	return v;
}

/* RES is read-only. */
static void
adc_res_write(struct avr_t * avr, avr_io_addr_t addr, uint8_t v, void * param)
{
	(void)avr; (void)addr; (void)v; (void)param;
}

static void
adc_irq_input(struct avr_irq_t * irq, uint32_t value, void * param)
{
	avr_adc_dd_t * p = (avr_adc_dd_t *)param;
	int idx = irq->irq - p->base_irq;

	if (idx == AVR_ADC_DD_CH_TEMPSENSE)
		p->temp_k = (int32_t)value;
	else if (idx < AVR_ADC_DD_NCHANNELS)
		p->chan_mv[idx] = value & 0xffff;
	else if (idx == AVR_ADC_DD_IRQ_VDD)
		p->vdd_mv = value;
	else if (idx == AVR_ADC_DD_IRQ_VDDIO2)
		p->vddio2_mv = value;
	else if (idx == AVR_ADC_DD_IRQ_REF)
		p->ref_mv = value;
}

void avr_adc_dd_set_ref(avr_adc_dd_t * p, uint32_t ref_mv) { p->ref_mv = ref_mv; }
void avr_adc_dd_set_vdd(avr_adc_dd_t * p, uint32_t vdd_mv) { p->vdd_mv = vdd_mv; }
void avr_adc_dd_set_tempsense(avr_adc_dd_t * p, avr_io_addr_t a) { p->r_tempcal = a; }
void avr_adc_dd_set_temp_k(avr_adc_dd_t * p, int32_t kelvin) { p->temp_k = kelvin; }
void avr_adc_dd_set_dacref_source(avr_adc_dd_t * p, const uint32_t * mv) { p->dacref_mv = mv; }

void
avr_adc_dd_event_start(avr_adc_dd_t * p)
{
	if ((reg(p, ADCDD_EVCTRL) & STARTEI_bm) && (reg(p, ADCDD_CTRLA) & ENABLE_bm) && !p->busy)
		adc_start(p);
}

static void
adc_reset(avr_io_t * io)
{
	avr_adc_dd_t * p = (avr_adc_dd_t *)io;

	avr_cycle_timer_cancel(p->io.avr, adc_done, p);
	memset(&p->io.avr->data[p->base], 0, 0x16);
	p->busy = 0;
	p->need_init = 0;
	p->ref_mv = 1024;		// VREF.ADC0REF resets to 1V024
}

static const char * irq_names[AVR_ADC_DD_IRQ_COUNT];

static avr_io_t _io = {
	.kind = "adc_dd",
	.reset = adc_reset,
};

void
avr_adc_dd_init(
		avr_t * avr,
		avr_adc_dd_t * p,
		avr_io_addr_t base,
		uint8_t vec_resrdy,
		uint8_t vec_wcmp,
		char name)
{
	static char names[AVR_ADC_DD_IRQ_COUNT][12];

	memset(p, 0, sizeof(*p));
	p->io = _io;
	p->name = name;
	p->base = base;
	p->vdd_mv = 3300;
	p->vddio2_mv = 3300;
	p->ref_mv = 1024;
	p->temp_k = 298;		// 25 C

	for (int i = 0; i < AVR_ADC_DD_IRQ_COUNT; i++) {
		if (i == AVR_ADC_DD_IRQ_EVT_RES)
			strcpy(names[i], ">adc.res");
		else if (i < AVR_ADC_DD_NCHANNELS)
			snprintf(names[i], sizeof(names[i]), "adc.in%02x", i);
		else
			snprintf(names[i], sizeof(names[i]), "adc.x%02x", i);
		irq_names[i] = names[i];
	}
	p->io.irq_names = irq_names;

	p->resrdy.vector = vec_resrdy;
	p->resrdy.enable.reg = base + ADCDD_INTCTRL;	p->resrdy.enable.mask = RESRDY_bm;
	p->resrdy.raised.reg = base + ADCDD_INTFLAGS;	p->resrdy.raised.mask = RESRDY_bm;
	p->resrdy.raise_sticky = 1;
	p->wcmp.vector = vec_wcmp;
	p->wcmp.enable.reg = base + ADCDD_INTCTRL;		p->wcmp.enable.mask = WCMP_bm;
	p->wcmp.raised.reg = base + ADCDD_INTFLAGS;		p->wcmp.raised.mask = WCMP_bm;
	p->wcmp.raise_sticky = 1;

	avr_register_io(avr, &p->io);
	avr_register_vector(avr, &p->resrdy);
	avr_register_vector(avr, &p->wcmp);
	avr_io_setirqs(&p->io, AVR_IOCTL_ADC_DD_GETIRQ(name), AVR_ADC_DD_IRQ_COUNT, NULL);
	p->base_irq = p->io.irq[0].irq;
	for (int i = 0; i < AVR_ADC_DD_IRQ_COUNT; i++)
		if (i != AVR_ADC_DD_IRQ_EVT_RES)
			avr_irq_register_notify(p->io.irq + i, adc_irq_input, p);

	avr_register_io_write(avr, base + ADCDD_CTRLA, adc_ctrla_write, p);
	avr_register_io_write(avr, base + ADCDD_COMMAND, adc_command_write, p);
	avr_register_io_write(avr, base + ADCDD_INTFLAGS, adc_intflags_write, p);
	for (int i = 0; i < 2; i++) {
		avr_register_io_read(avr, base + ADCDD_RES + i, adc_res_read, p);
		avr_register_io_write(avr, base + ADCDD_RES + i, adc_res_write, p);
	}
}
