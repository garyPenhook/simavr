/*
	avr_adc_tiny2.c

	tinyAVR(R) 2-series 12-bit differential ADC with PGA. See avr_adc_tiny2.h.

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

#include <stdio.h>
#include <string.h>
#include "avr_adc_tiny2.h"
#include "sim_cycle_timers.h"

/* CTRLA */
#define ENABLE_bm	0x01
#define LOWLAT_bm	0x20

/* CTRLB.PRESC[3:0]: CLK_PER divisor (DS40002311B 30.5.2). */
static const uint8_t presc_div[16] = {
	2, 4, 6, 8, 10, 12, 14, 16, 20, 24, 28, 32, 40, 48, 56, 64
};

/* CTRLC */
#define REFSEL_gm	0x07

/* CTRLD */
#define WINCM_gm	0x07
#define WINSRC_bm	0x08	/* 0 = RESULT, 1 = SAMPLE */
#define WINCM_NONE	0
#define WINCM_BELOW	1
#define WINCM_ABOVE	2
#define WINCM_INSIDE	3
#define WINCM_OUTSIDE	4

/* INTCTRL / INTFLAGS share one layout. */
#define F_RESRDY	0x01
#define F_SAMPRDY	0x02
#define F_WCMP		0x04
#define F_RESOVR	0x08
#define F_SAMPOVR	0x10
#define F_TRIGOVR	0x20
#define F_ALL		0x3f

/* STATUS */
#define ADCBUSY_bm	0x01

/* CTRLF */
#define SAMPNUM_gm	0x0f
#define LEFTADJ_bm	0x10
#define FREERUN_bm	0x20

/* COMMAND */
#define DIFF_bm		0x80
#define MODE_gm		0x70
#define MODE_gp		4
#define START_gm	0x07
#define MODE_SINGLE_8BIT	0
#define MODE_SINGLE_12BIT	1
#define MODE_SERIES		2
#define MODE_SERIES_SCALING	3
#define MODE_BURST		4
#define MODE_BURST_SCALING	5
#define START_STOP		0
#define START_IMMEDIATE		1
#define START_MUXPOS_WRITE	2
#define START_MUXNEG_WRITE	3
#define START_EVENT		4

/* PGACTRL */
#define PGAEN_bm	0x01
#define PGASAMPDUR_gm	0x06
#define PGASAMPDUR_gp	1
#define GAIN_gm		0xe0
#define GAIN_gp		5
#define PGACTRL_RESET	0x04	/* ADCPGASAMPDUR = 20CLK */

/* MUXPOS / MUXNEG */
#define VIA_gm		0xc0
#define VIA_PGA		0x40
#define MUX_gm		0x3f

/* Analog modules needing initialisation time (DS40002311B Table 30-5). */
#define M_ADC		0x01
#define M_PGA		0x02
#define M_REF		0x04
#define M_TEMP		0x08
#define M_DAC		0x10

static inline uint8_t rd(avr_t *avr, avr_io_addr_t a) { return avr->data[a]; }
static inline avr_io_addr_t reg(avr_adc_tiny2_t *p, int off) { return p->base + off; }

static int adc_enabled(avr_adc_tiny2_t *p)
{
	return (rd(p->io.avr, reg(p, ADC2R_CTRLA)) & ENABLE_bm) != 0;
}

static uint8_t adc_mode(avr_adc_tiny2_t *p)
{
	return (rd(p->io.avr, reg(p, ADC2R_COMMAND)) & MODE_gm) >> MODE_gp;
}

static int adc_is_series(uint8_t mode)
{
	return mode == MODE_SERIES || mode == MODE_SERIES_SCALING;
}

static int adc_is_burst(uint8_t mode)
{
	return mode == MODE_BURST || mode == MODE_BURST_SCALING;
}

/* CTRLF.SAMPNUM, clamped to the defined range (reserved values -> ACC1024). */
static uint8_t adc_sampnum_field(avr_adc_tiny2_t *p)
{
	uint8_t n = rd(p->io.avr, reg(p, ADC2R_CTRLF)) & SAMPNUM_gm;
	return n > 10 ? 10 : n;
}

/* The PGA is in the signal path when enabled and selected by MUXPOS.VIA. */
static int adc_pga_used(avr_adc_tiny2_t *p)
{
	avr_t *avr = p->io.avr;
	return (rd(avr, reg(p, ADC2R_PGACTRL)) & PGAEN_bm) &&
		   (rd(avr, reg(p, ADC2R_MUXPOS)) & VIA_gm) == VIA_PGA;
}

static uint32_t adc_pga_gain(avr_adc_tiny2_t *p)
{
	uint8_t g = (rd(p->io.avr, reg(p, ADC2R_PGACTRL)) & GAIN_gm) >> GAIN_gp;
	return g > 4 ? 16 : 1u << g;	/* 1x..16x; reserved codes saturate */
}

/* ADCPGASAMPDUR in CLK_ADC cycles (PGACTRL[2:1]: 6 / 15 / 20). */
static uint32_t adc_pga_sampdur(avr_adc_tiny2_t *p)
{
	switch ((rd(p->io.avr, reg(p, ADC2R_PGACTRL)) & PGASAMPDUR_gm) >> PGASAMPDUR_gp) {
	case 0: return 6;
	case 1: return 15;
	default: return 20;
	}
}

static uint32_t adc_ref_mv(avr_adc_tiny2_t *p)
{
	switch (rd(p->io.avr, reg(p, ADC2R_CTRLC)) & REFSEL_gm) {
	case 0x2: return p->vrefa_mv;
	case 0x4: return 1024;
	case 0x5: return 2048;
	case 0x6: return 2500;
	case 0x7: return 4096;
	default: return p->vdd_mv;	/* VDD; reserved 0x1/0x3 fall back to VDD */
	}
}

/* ---- timing (DS40002311B 30.3.3.6) -------------------------------------- */

/* CPU cycles for 'half' half-ADC-clock periods (CLK_PER = CPU clock). */
static avr_cycle_count_t adc_cycles(avr_adc_tiny2_t *p, uint32_t half)
{
	uint32_t div = presc_div[rd(p->io.avr, reg(p, ADC2R_CTRLB)) & 0x0f];
	avr_cycle_count_t c = ((avr_cycle_count_t)half * div + 1) / 2;
	return c ? c : 1;
}

/* Modules the current configuration needs initialised. */
static uint8_t adc_modules_used(avr_adc_tiny2_t *p)
{
	avr_t *avr = p->io.avr;
	uint8_t pos = rd(avr, reg(p, ADC2R_MUXPOS)) & MUX_gm;
	uint8_t neg = rd(avr, reg(p, ADC2R_MUXNEG)) & MUX_gm;
	int diff = (rd(avr, reg(p, ADC2R_COMMAND)) & DIFF_bm) != 0;
	uint8_t m = M_ADC;

	if (adc_pga_used(p))
		m |= M_PGA;
	if ((rd(avr, reg(p, ADC2R_CTRLC)) & REFSEL_gm) >= 4)
		m |= M_REF;
	if (pos == AVR_ADC2_CH_TEMPSENSE)
		m |= M_TEMP;
	if (pos == AVR_ADC2_CH_DACREF0 || (diff && neg == AVR_ADC2_CH_DACREF0))
		m |= M_DAC;
	return m;
}

/* Initialisation time (CPU cycles) for the not-yet-warm modules: the modules
 * start in parallel, so the slowest one sets the delay (Table 30-5). */
static avr_cycle_count_t adc_init_cycles(avr_adc_tiny2_t *p)
{
	uint8_t need = adc_modules_used(p) & ~p->warm;
	uint32_t us = 0;

	if (need & M_ADC)
		us = 10;
	if (need & M_PGA && us < 20)
		us = 20;
	if (need & M_TEMP && us < 35)
		us = 35;
	if (need & M_DAC && us < 35)
		us = 35;
	if (need & M_REF)
		us = 60;
	if (!us)
		return 0;
	/* Below 2 MHz the ADC itself needs 20 CLK_PER cycles instead of 10 us. */
	if (us == 10 && p->io.avr->frequency < 2000000)
		return 20;
	return (avr_cycle_count_t)us * p->io.avr->frequency / 1000000;
}

/* ADC clocks (x2, i.e. half clocks) from trigger to the *first* sample ready,
 * excluding initialisation. SAMPDUR is CTRLE. */
static uint32_t adc_first_sample_half(avr_adc_tiny2_t *p, uint8_t mode)
{
	avr_t *avr = p->io.avr;
	uint32_t sampdur = rd(avr, reg(p, ADC2R_CTRLE));
	int pga = adc_pga_used(p);

	if (adc_is_burst(mode)) {
		if (pga)	/* (SAMPDUR + ADCPGASAMPDUR + 2) + 14 for the first sample */
			return 2 * (sampdur + adc_pga_sampdur(p) + 2 + 14);
		return 2 * (sampdur + 14) + 3;	/* (SAMPDUR + 14) + 1.5 */
	}
	/* Single / Series: 12-bit SAMPDUR+15, 8-bit SAMPDUR+11 (+0.5 with PGA,
	 * plus ADCPGASAMPDUR). */
	uint32_t conv = (mode == MODE_SINGLE_8BIT) ? 11 : 15;
	if (pga)
		return 2 * (sampdur + adc_pga_sampdur(p) + conv) + 1;
	return 2 * (sampdur + conv);
}

/* Half clocks between successive burst samples. */
static uint32_t adc_burst_step_half(avr_adc_tiny2_t *p)
{
	uint32_t sampdur = rd(p->io.avr, reg(p, ADC2R_CTRLE));
	if (adc_pga_used(p))
		return 2 * (sampdur + adc_pga_sampdur(p) + 2);
	return 2 * (sampdur + 14);
}

/* ---- interrupts ---------------------------------------------------------- */

static uint8_t adc_vec_mask(avr_adc_tiny2_t *p, const avr_int_vector_t *v)
{
	uint8_t wsrc = (rd(p->io.avr, reg(p, ADC2R_CTRLD)) & WINSRC_bm) != 0;

	if (v == &p->error)
		return F_TRIGOVR | F_SAMPOVR | F_RESOVR;
	if (v == &p->samprdy)
		return F_SAMPRDY | (wsrc ? F_WCMP : 0);
	return F_RESRDY | (wsrc ? 0 : F_WCMP);
}

static int adc_vec_active(avr_adc_tiny2_t *p, const avr_int_vector_t *v)
{
	avr_t *avr = p->io.avr;
	return (rd(avr, reg(p, ADC2R_INTFLAGS)) & rd(avr, reg(p, ADC2R_INTCTRL)) &
			adc_vec_mask(p, v)) != 0;
}

/* Request every vector with an enabled, set flag; drop those with none. */
static void adc_irq_sync(avr_adc_tiny2_t *p, int raise)
{
	avr_t *avr = p->io.avr;
	avr_int_vector_t *vecs[3] = { &p->error, &p->samprdy, &p->resrdy };

	for (int i = 0; i < 3; i++) {
		if (adc_vec_active(p, vecs[i])) {
			if (raise)
				avr_raise_interrupt(avr, vecs[i]);
		} else {
			avr_clear_interrupt(avr, vecs[i]);
		}
	}
}

static void adc_set_flags(avr_adc_tiny2_t *p, uint8_t flags)
{
	avr_t *avr = p->io.avr;
	avr_core_watch_write(avr, reg(p, ADC2R_INTFLAGS),
						 rd(avr, reg(p, ADC2R_INTFLAGS)) | flags);
	adc_irq_sync(p, 1);
}

static void adc_pulse(avr_adc_tiny2_t *p, int evt)
{
	avr_raise_irq(p->io.irq + evt, 1);
	avr_raise_irq(p->io.irq + evt, 0);
}

/* ---- conversion ---------------------------------------------------------- */

static int64_t floor_div(int64_t a, int64_t b)
{
	int64_t q = a / b;
	return ((a % b) != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

static int64_t adc_input_mv(avr_adc_tiny2_t *p, uint8_t ch)
{
	switch (ch) {
	case 0x00:
	case AVR_ADC2_CH_GND:		return 0;
	case AVR_ADC2_CH_VDDDIV10:	return p->vdd_mv / 10;
	case AVR_ADC2_CH_DACREF0:	return p->dacref_mv ? *p->dacref_mv : 0;
	default:			return ch < AVR_ADC2_NCHANNELS ? p->chan_mv[ch] : 0;
	}
}

/* 12-bit code the temperature sensor presents: the datasheet's firmware
 * computes T = ((RESULT >> 2) - offset) * gain / 256 (30.3.3.7), so return the
 * 10-bit code that inverts that, aligned into 12 bits. */
static int32_t adc_temp_code12(avr_adc_tiny2_t *p)
{
	avr_t *avr = p->io.avr;
	uint8_t gain = avr->data[p->r_tempcal];
	int8_t off = (int8_t)avr->data[p->r_tempcal + 1];
	if (gain == 0)
		return 0;
	int32_t code = off + (p->temp_k * 256 + gain / 2) / gain;
	if (code < 0)
		code = 0;
	return (code > 1023 ? 1023 : code) << 2;
}

/* One conversion of the selected input(s): unsigned for single-ended, two's
 * complement for differential (DS40002311B 30.3.3.3). */
static int32_t adc_convert(avr_adc_tiny2_t *p, int bits8, int diff)
{
	avr_t *avr = p->io.avr;
	uint8_t pos = rd(avr, reg(p, ADC2R_MUXPOS)) & MUX_gm;
	uint8_t neg = rd(avr, reg(p, ADC2R_MUXNEG)) & MUX_gm;

	if (!diff && pos == AVR_ADC2_CH_TEMPSENSE && p->r_tempcal) {
		int32_t c12 = adc_temp_code12(p);
		return bits8 ? c12 >> 4 : c12;
	}

	int64_t vp = adc_input_mv(p, pos);
	int64_t vn = diff ? adc_input_mv(p, neg) : 0;
	int64_t gain = adc_pga_used(p) ? adc_pga_gain(p) : 1;
	int64_t ref = adc_ref_mv(p);
	if (ref == 0)
		ref = 1;

	if (!diff) {
		int64_t full = bits8 ? 256 : 4096;
		int64_t c = floor_div(vp * gain * full, ref);
		return (int32_t)(c < 0 ? 0 : c > full - 1 ? full - 1 : c);
	} else {
		int64_t full = bits8 ? 128 : 2048;
		int64_t c = floor_div((vp - vn) * gain * full, ref);
		return (int32_t)(c < -full ? -full : c > full - 1 ? full - 1 : c);
	}
}

static void adc_write16(avr_adc_tiny2_t *p, int off, uint16_t v)
{
	avr_core_watch_write(p->io.avr, reg(p, off), v & 0xff);
	avr_core_watch_write(p->io.avr, reg(p, off + 1), v >> 8);
}

static void adc_write32(avr_adc_tiny2_t *p, int off, uint32_t v)
{
	for (int i = 0; i < 4; i++)
		avr_core_watch_write(p->io.avr, reg(p, off + i), (v >> (8 * i)) & 0xff);
}

/* Window comparator on the 16 LSb of 'out' (DS40002311B 30.3.3.8, 30.5.4). The
 * thresholds share the data format, so differential values compare signed. */
static void adc_window_compare(avr_adc_tiny2_t *p, uint16_t out, int diff)
{
	avr_t *avr = p->io.avr;
	uint16_t ltr = rd(avr, reg(p, ADC2R_WINLT)) | (rd(avr, reg(p, ADC2R_WINLT + 1)) << 8);
	uint16_t htr = rd(avr, reg(p, ADC2R_WINHT)) | (rd(avr, reg(p, ADC2R_WINHT + 1)) << 8);
	int32_t o = diff ? (int16_t)out : out;
	int32_t lt = diff ? (int16_t)ltr : ltr;
	int32_t ht = diff ? (int16_t)htr : htr;
	int hit = 0;

	switch (rd(avr, reg(p, ADC2R_CTRLD)) & WINCM_gm) {
	case WINCM_BELOW:	hit = o < lt; break;
	case WINCM_ABOVE:	hit = o > ht; break;
	case WINCM_INSIDE:	hit = o > lt && o < ht; break;
	case WINCM_OUTSIDE:	hit = o < lt || o > ht; break;
	default:		return;
	}
	if (hit) {
		adc_set_flags(p, F_WCMP);
		adc_pulse(p, AVR_ADC2_IRQ_EVT_WCMP);
	}
}

/* Publish a finished sample (every conversion). */
static void adc_publish_sample(avr_adc_tiny2_t *p, int32_t code, uint8_t mode,
							   int diff)
{
	avr_t *avr = p->io.avr;
	int leftadj = (rd(avr, reg(p, ADC2R_CTRLF)) & LEFTADJ_bm) != 0;
	int32_t s = code;
	uint8_t flags = F_SAMPRDY;

	/* Table 30-4: 8-bit samples are never shifted; 12-bit follow LEFTADJ. */
	if (mode != MODE_SINGLE_8BIT && leftadj)
		s = code * 16;
	if (rd(avr, reg(p, ADC2R_INTFLAGS)) & F_SAMPRDY)
		flags |= F_SAMPOVR;		/* unread sample overwritten */
	adc_write16(p, ADC2R_SAMPLE, (uint16_t)s);
	adc_set_flags(p, flags);
	adc_pulse(p, AVR_ADC2_IRQ_EVT_SAMP);

	if (rd(avr, reg(p, ADC2R_CTRLD)) & WINSRC_bm)
		adc_window_compare(p, (uint16_t)s, diff);
}

/* Publish the RESULT (single: every conversion; accumulation: when complete). */
static void adc_publish_result(avr_adc_tiny2_t *p, int32_t code, uint8_t mode,
							   int diff)
{
	avr_t *avr = p->io.avr;
	int leftadj = (rd(avr, reg(p, ADC2R_CTRLF)) & LEFTADJ_bm) != 0;
	uint8_t n = adc_sampnum_field(p);
	int64_t r;
	uint8_t flags = F_RESRDY;

	switch (mode) {
	case MODE_SINGLE_8BIT:
		r = code;
		break;
	case MODE_SERIES:
	case MODE_BURST:		/* full accumulation, no LEFTADJ (Table 30-2 n.1) */
		r = p->acc_sum;
		break;
	case MODE_SERIES_SCALING:
	case MODE_BURST_SCALING:	/* >> (SAMPNUM-4); <4 samples left-justify */
		r = p->acc_sum;
		if (n > 4)
			r >>= (n - 4);
		else if (leftadj)
			r <<= (4 - n);
		break;
	default:			/* MODE_SINGLE_12BIT */
		r = leftadj ? (int64_t)code * 16 : code;
		break;
	}

	if (rd(avr, reg(p, ADC2R_INTFLAGS)) & F_RESRDY)
		flags |= F_RESOVR;		/* unread result overwritten */
	adc_write32(p, ADC2R_RESULT, (uint32_t)(int32_t)r);
	adc_set_flags(p, flags);
	adc_pulse(p, AVR_ADC2_IRQ_EVT_RES);

	if (!(rd(avr, reg(p, ADC2R_CTRLD)) & WINSRC_bm))
		adc_window_compare(p, (uint16_t)r, diff);
}

static void adc_set_busy(avr_adc_tiny2_t *p, int busy)
{
	p->busy = busy;
	avr_core_watch_write(p->io.avr, reg(p, ADC2R_STATUS), busy ? ADCBUSY_bm : 0);
}

static avr_cycle_count_t
avr_adc_tiny2_sample_done(struct avr_t *avr, avr_cycle_count_t when, void *param)
{
	avr_adc_tiny2_t *p = (avr_adc_tiny2_t *)param;
	uint8_t cmd = rd(avr, reg(p, ADC2R_COMMAND));
	uint8_t mode = (cmd & MODE_gm) >> MODE_gp;
	int diff = (cmd & DIFF_bm) != 0;
	int bits8 = mode == MODE_SINGLE_8BIT;

	if (!adc_enabled(p)) {
		adc_set_busy(p, 0);
		return 0;
	}

	int32_t code = adc_convert(p, bits8, diff);
	adc_publish_sample(p, code, mode, diff);

	if (adc_is_series(mode) || adc_is_burst(mode)) {
		p->acc_sum += code;
		if (adc_is_burst(mode))
			p->burst_done++;
		else
			p->acc_count++;
	}

	if (adc_is_burst(mode)) {
		if (p->burst_done < (1u << adc_sampnum_field(p)))
			return when + adc_cycles(p, adc_burst_step_half(p));
		adc_publish_result(p, code, mode, diff);
		p->acc_sum = 0;
		p->burst_done = 0;
	} else if (adc_is_series(mode)) {
		if (p->acc_count >= (1u << adc_sampnum_field(p))) {
			adc_publish_result(p, code, mode, diff);
			p->acc_sum = 0;
			p->acc_count = 0;
		}
	} else {
		adc_publish_result(p, code, mode, diff);
	}

	/* Modules stay powered after the conversion only with LOWLAT. */
	p->warm = (rd(avr, reg(p, ADC2R_CTRLA)) & LOWLAT_bm) ?
			  (p->warm | adc_modules_used(p)) : 0;

	/* FREERUN restarts Single and Burst immediately (no initialisation);
	 * it is ignored in Series mode. */
	if (p->freerun_active && !adc_is_series(mode) &&
		(rd(avr, reg(p, ADC2R_CTRLF)) & FREERUN_bm) &&
		(cmd & START_gm) != START_STOP) {
		p->burst_done = 0;
		return when + adc_cycles(p, adc_first_sample_half(p, mode));
	}

	/* START=IMMEDIATE returns to STOP once the conversion is done. */
	p->freerun_active = 0;
	if ((cmd & START_gm) == START_IMMEDIATE)
		avr_core_watch_write(avr, reg(p, ADC2R_COMMAND), cmd & ~START_gm);
	adc_set_busy(p, 0);
	return 0;
}

static void adc_abort(avr_adc_tiny2_t *p)
{
	avr_cycle_timer_cancel(p->io.avr, avr_adc_tiny2_sample_done, p);
	p->freerun_active = 0;
	p->burst_done = 0;
	adc_set_busy(p, 0);
}

/* A start trigger (immediate, MUXPOS/MUXNEG write or event). */
static void adc_trigger(avr_adc_tiny2_t *p)
{
	avr_t *avr = p->io.avr;
	uint8_t mode = adc_mode(p);

	if (!adc_enabled(p))
		return;
	if (p->busy) {		/* trigger while converting: flag it and ignore it */
		adc_set_flags(p, F_TRIGOVR);
		return;
	}
	/* Series keeps accumulating across triggers; everything else restarts. */
	if (!adc_is_series(mode)) {
		p->acc_sum = 0;
		p->acc_count = 0;
	} else if (p->acc_count == 0) {
		p->acc_sum = 0;
	}
	p->burst_done = 0;
	p->freerun_active = !adc_is_series(mode) &&
						(rd(avr, reg(p, ADC2R_CTRLF)) & FREERUN_bm);

	adc_set_busy(p, 1);
	avr_cycle_timer_register(avr,
			adc_init_cycles(p) + adc_cycles(p, adc_first_sample_half(p, mode)),
			avr_adc_tiny2_sample_done, p);
}

/* ---- register hooks ------------------------------------------------------ */

static void
avr_adc_tiny2_ctrla_write(struct avr_t *avr, avr_io_addr_t addr, uint8_t v,
						  void *param)
{
	avr_adc_tiny2_t *p = (avr_adc_tiny2_t *)param;
	avr_core_watch_write(avr, addr, v);
	if (!(v & ENABLE_bm)) {		/* disabling aborts and powers the analog down */
		adc_abort(p);
		p->warm = 0;
	}
}

static void
avr_adc_tiny2_command_write(struct avr_t *avr, avr_io_addr_t addr, uint8_t v,
							void *param)
{
	avr_adc_tiny2_t *p = (avr_adc_tiny2_t *)param;
	uint8_t old_mode = adc_mode(p);
	uint8_t start = v & START_gm;

	/* IMMEDIATE with the ADC disabled reads back as STOP (30.5.11). */
	if (start == START_IMMEDIATE && !adc_enabled(p))
		v &= ~START_gm, start = START_STOP;
	avr_core_watch_write(avr, addr, v);

	/* Leaving an accumulation mode for Single resets the accumulator. */
	if (!(old_mode < MODE_SERIES) && ((v & MODE_gm) >> MODE_gp) < MODE_SERIES) {
		p->acc_sum = 0;
		p->acc_count = 0;
	}

	if (start == START_STOP)
		adc_abort(p);
	else if (start == START_IMMEDIATE)
		adc_trigger(p);
}

/* MUXPOS.VIA and MUXNEG.VIA are one shared field: mirror it into the other. */
static void
avr_adc_tiny2_muxpos_write(struct avr_t *avr, avr_io_addr_t addr, uint8_t v,
						   void *param)
{
	avr_adc_tiny2_t *p = (avr_adc_tiny2_t *)param;
	avr_io_addr_t other = reg(p, ADC2R_MUXNEG);
	avr_core_watch_write(avr, addr, v);
	avr_core_watch_write(avr, other, (rd(avr, other) & ~VIA_gm) | (v & VIA_gm));
	if ((rd(avr, reg(p, ADC2R_COMMAND)) & START_gm) == START_MUXPOS_WRITE)
		adc_trigger(p);
}

static void
avr_adc_tiny2_muxneg_write(struct avr_t *avr, avr_io_addr_t addr, uint8_t v,
						   void *param)
{
	avr_adc_tiny2_t *p = (avr_adc_tiny2_t *)param;
	avr_io_addr_t other = reg(p, ADC2R_MUXPOS);
	avr_core_watch_write(avr, addr, v);
	avr_core_watch_write(avr, other, (rd(avr, other) & ~VIA_gm) | (v & VIA_gm));
	if ((rd(avr, reg(p, ADC2R_COMMAND)) & START_gm) == START_MUXNEG_WRITE)
		adc_trigger(p);
}

static void
avr_adc_tiny2_intctrl_write(struct avr_t *avr, avr_io_addr_t addr, uint8_t v,
							void *param)
{
	avr_adc_tiny2_t *p = (avr_adc_tiny2_t *)param;
	avr_core_watch_write(avr, addr, v & F_ALL);
	adc_irq_sync(p, 1);	/* enabling a flag that is already set requests it */
}

static void
avr_adc_tiny2_intflags_write(struct avr_t *avr, avr_io_addr_t addr, uint8_t v,
							 void *param)
{
	avr_adc_tiny2_t *p = (avr_adc_tiny2_t *)param;
	avr_core_watch_write(avr, addr, avr->data[addr] & ~(v & F_ALL));	/* W1C */
	adc_irq_sync(p, 0);
}

/* STATUS, RESULT and SAMPLE are read-only. */
static void
avr_adc_tiny2_ro_write(struct avr_t *avr, avr_io_addr_t addr, uint8_t v,
					   void *param)
{
	(void)avr; (void)addr; (void)v; (void)param;
}

/* Reading RESULT / SAMPLE (low byte) clears RESRDY / SAMPRDY (30.5.6). */
static uint8_t
avr_adc_tiny2_result_read(struct avr_t *avr, avr_io_addr_t addr, void *param)
{
	avr_adc_tiny2_t *p = (avr_adc_tiny2_t *)param;
	avr_io_addr_t f = reg(p, ADC2R_INTFLAGS);
	avr_core_watch_write(avr, f, rd(avr, f) & ~F_RESRDY);
	adc_irq_sync(p, 0);
	return avr->data[addr];
}

static uint8_t
avr_adc_tiny2_sample_read(struct avr_t *avr, avr_io_addr_t addr, void *param)
{
	avr_adc_tiny2_t *p = (avr_adc_tiny2_t *)param;
	avr_io_addr_t f = reg(p, ADC2R_INTFLAGS);
	avr_core_watch_write(avr, f, rd(avr, f) & ~F_SAMPRDY);
	adc_irq_sync(p, 0);
	return avr->data[addr];
}

/* A board/test presents the analog voltage (mV) on an input; the temperature
 * input carries the die temperature in Kelvin. The VDD / VREFA IRQs set the
 * supply and external-reference voltages. */
static void
avr_adc_tiny2_irq_input(struct avr_irq_t *irq, uint32_t value, void *param)
{
	avr_adc_tiny2_t *p = (avr_adc_tiny2_t *)param;
	int ch = irq->irq - p->base_irq;

	if (ch == AVR_ADC2_IRQ_VDD)
		p->vdd_mv = value;
	else if (ch == AVR_ADC2_IRQ_VREFA)
		p->vrefa_mv = value;
	else if (ch == AVR_ADC2_CH_TEMPSENSE)
		p->temp_k = (int32_t)value;
	else if (ch >= 0 && ch < AVR_ADC2_NCHANNELS &&
			 ch != AVR_ADC2_CH_GND && ch != AVR_ADC2_CH_VDDDIV10 &&
			 ch != AVR_ADC2_CH_DACREF0)
		p->chan_mv[ch] = value > 0xffff ? 0xffff : value;
}

static void
avr_adc_tiny2_reset(avr_io_t *io)
{
	avr_adc_tiny2_t *p = (avr_adc_tiny2_t *)io;
	avr_t *avr = p->io.avr;

	avr_cycle_timer_cancel(avr, avr_adc_tiny2_sample_done, p);
	p->busy = 0;
	p->freerun_active = 0;
	p->warm = 0;
	p->acc_sum = 0;
	p->acc_count = 0;
	p->burst_done = 0;
	avr->data[reg(p, ADC2R_PGACTRL)] = PGACTRL_RESET;
}

static const char *irq_names[AVR_ADC2_IRQ_COUNT] = {
	"adc.ain0", "adc.ain1", "adc.ain2", "adc.ain3",
	"adc.ain4", "adc.ain5", "adc.ain6", "adc.ain7",
	"adc.ain8", "adc.ain9", "adc.ain10", "adc.ain11",
	"adc.ain12", "adc.ain13", "adc.ain14", "adc.ain15",
	"adc.ch0x10", "adc.ch0x11", "adc.ch0x12", "adc.ch0x13",
	"adc.ch0x14", "adc.ch0x15", "adc.ch0x16", "adc.ch0x17",
	"adc.ch0x18", "adc.ch0x19", "adc.ch0x1a", "adc.ch0x1b",
	"adc.ch0x1c", "adc.ch0x1d", "adc.ch0x1e", "adc.ch0x1f",
	"adc.ch0x20", "adc.ch0x21", "adc.ch0x22", "adc.ch0x23",
	"adc.ch0x24", "adc.ch0x25", "adc.ch0x26", "adc.ch0x27",
	"adc.ch0x28", "adc.ch0x29", "adc.ch0x2a", "adc.ch0x2b",
	"adc.ch0x2c", "adc.ch0x2d", "adc.ch0x2e", "adc.ch0x2f",
	"adc.gnd", "adc.vdddiv10", "adc.temp", "adc.dacref0",
	">adc.evt.res", ">adc.evt.samp", ">adc.evt.wcmp",
	"adc.vdd", "adc.vrefa",
};

static avr_io_t _io = {
	.kind = "adc_tiny2",
	.reset = avr_adc_tiny2_reset,
	.irq_names = irq_names,
};

void
avr_adc_tiny2_set_vdd(avr_adc_tiny2_t * p, uint32_t vdd_mv)
{
	p->vdd_mv = vdd_mv;
}

void
avr_adc_tiny2_set_vrefa(avr_adc_tiny2_t * p, uint32_t vrefa_mv)
{
	p->vrefa_mv = vrefa_mv;
}

void
avr_adc_tiny2_set_tempsense(avr_adc_tiny2_t * p, avr_io_addr_t tempcal_addr)
{
	p->r_tempcal = tempcal_addr;
}

void
avr_adc_tiny2_set_temp_k(avr_adc_tiny2_t * p, int32_t kelvin)
{
	p->temp_k = kelvin;
}

void
avr_adc_tiny2_set_dacref_source(avr_adc_tiny2_t * p, const uint32_t * dacref_mv)
{
	p->dacref_mv = dacref_mv;
}

void
avr_adc_tiny2_event_start(avr_adc_tiny2_t * p)
{
	if ((rd(p->io.avr, reg(p, ADC2R_COMMAND)) & START_gm) == START_EVENT)
		adc_trigger(p);
}

static void
adc_init_vector(avr_adc_tiny2_t * p, avr_int_vector_t * v, uint8_t vector,
				uint8_t en_bit, uint8_t en_mask)
{
	/* The request is raised only for an enabled, set flag (adc_irq_sync), so
	 * the engine's gate is permissive and there is no single 'raised' bit. */
	v->vector = vector;
	v->enable.reg = reg(p, ADC2R_INTCTRL);
	v->enable.bit = en_bit;
	v->enable.mask = en_mask;
	v->raise_sticky = 1;
}

void
avr_adc_tiny2_init(
		avr_t * avr,
		avr_adc_tiny2_t * p,
		avr_io_addr_t base,
		uint8_t vec_error,
		uint8_t vec_resrdy,
		uint8_t vec_samprdy,
		char name)
{
	memset(p, 0, sizeof(*p));
	p->io = _io;
	p->name = name;
	p->base = base;
	p->vdd_mv = 3300;
	p->vrefa_mv = 3300;
	p->temp_k = 298;	/* ~25 degC until the board sets a die temperature */

	adc_init_vector(p, &p->resrdy, vec_resrdy, 0, 0x05);	/* RESRDY | WCMP */
	adc_init_vector(p, &p->samprdy, vec_samprdy, 1, 0x03);	/* SAMPRDY | WCMP */
	adc_init_vector(p, &p->error, vec_error, 3, 0x07);	/* RESOVR|SAMPOVR|TRIGOVR */

	avr_register_io(avr, &p->io);
	avr_register_vector(avr, &p->error);
	avr_register_vector(avr, &p->samprdy);
	avr_register_vector(avr, &p->resrdy);

	avr_io_setirqs(&p->io, AVR_IOCTL_ADC2_GETIRQ(name), AVR_ADC2_IRQ_COUNT, NULL);
	p->base_irq = p->io.irq[0].irq;
	for (int i = 0; i < AVR_ADC2_NCHANNELS; i++)
		avr_irq_register_notify(p->io.irq + i, avr_adc_tiny2_irq_input, p);
	avr_irq_register_notify(p->io.irq + AVR_ADC2_IRQ_VDD, avr_adc_tiny2_irq_input, p);
	avr_irq_register_notify(p->io.irq + AVR_ADC2_IRQ_VREFA, avr_adc_tiny2_irq_input, p);

	avr->data[reg(p, ADC2R_PGACTRL)] = PGACTRL_RESET;

	avr_register_io_write(avr, reg(p, ADC2R_CTRLA), avr_adc_tiny2_ctrla_write, p);
	avr_register_io_write(avr, reg(p, ADC2R_COMMAND), avr_adc_tiny2_command_write, p);
	avr_register_io_write(avr, reg(p, ADC2R_MUXPOS), avr_adc_tiny2_muxpos_write, p);
	avr_register_io_write(avr, reg(p, ADC2R_MUXNEG), avr_adc_tiny2_muxneg_write, p);
	avr_register_io_write(avr, reg(p, ADC2R_INTCTRL), avr_adc_tiny2_intctrl_write, p);
	avr_register_io_write(avr, reg(p, ADC2R_INTFLAGS), avr_adc_tiny2_intflags_write, p);
	avr_register_io_write(avr, reg(p, ADC2R_STATUS), avr_adc_tiny2_ro_write, p);
	for (int i = 0; i < 4; i++)
		avr_register_io_write(avr, reg(p, ADC2R_RESULT + i), avr_adc_tiny2_ro_write, p);
	for (int i = 0; i < 2; i++)
		avr_register_io_write(avr, reg(p, ADC2R_SAMPLE + i), avr_adc_tiny2_ro_write, p);
	avr_register_io_read(avr, reg(p, ADC2R_RESULT), avr_adc_tiny2_result_read, p);
	avr_register_io_read(avr, reg(p, ADC2R_SAMPLE), avr_adc_tiny2_sample_read, p);
}
