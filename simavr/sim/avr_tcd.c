/*
	avr_tcd.c

	"Modern" AVR (AVRxt) 12-bit Timer/Counter type D.
	See avr_tcd.h.

	A simavr cycle timer steps between compare boundaries and ramp ends. The
	model covers the four waveform-generation modes:
	- One Ramp
	- Two Ramp
	- Four Ramp
	- Dual Slope

	STATUS reads ENRDY|CMDRDY so the double-buffered enable/sync polling protocol
	passes. The TCD clock source is approximated as CLK_PER, then divided by
	SYNCPRES and CNTPRES.

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
#include "avr_tcd.h"
#include "sim_cycle_timers.h"

/* CTRLA */
#define ENABLE_bm	0x01
#define SYNCPRES_gm	0x06
#define SYNCPRES_gp	1
#define CNTPRES_gm	0x18
#define CNTPRES_gp	3
#define CLKSEL_gm	0x60
#define CLKSEL_gp	5
#define CLKSEL_OSC20M	0x0	/* unprescaled internal 16/20 MHz oscillator */
#define CLKSEL_EXTCLK	0x2	/* external clock (no pin modelled) */
#define CLKSEL_SYSCLK	0x3	/* System Clock = CLK_PER */

/* OSCCFG fuse: FREQSEL selects the OSC20M base (matches avr_clkctrl.c). */
#define FREQSEL_gm	0x03
#define FREQSEL_16MHZ	0x01

/* CTRLB */
#define WGMODE_gm	0x03
#define WGMODE_ONERAMP	0
#define WGMODE_TWORAMP	1
#define WGMODE_FOURRAMP	2
#define WGMODE_DUALSLOPE 3

/* FAULTCTRL */
#define CMPAEN_bm	0x10
#define CMPBEN_bm	0x20

/* INTCTRL / INTFLAGS */
#define OVF_bm		0x01

/* CTRLC / FAULTCTRL extras */
#define CMPCSEL_bm	0x40
#define CMPDSEL_bm	0x80
#define CMPCEN_bm	0x40
#define CMPDEN_bm	0x80

/* EVCTRLx */
#define TRIGEI_bm	0x01
#define EDGE_bm		0x10

/* CTRLE strobes (DS40002413B 25.5.5) */
#define RESTART_bm	0x04
#define SCAPTUREA_bm	0x08
#define SCAPTUREB_bm	0x10

/* EVCTRLx.ACTION */
#define ACTION_bm	0x04

/* INPUTCTRLx.INPUTMODE (25.5.11) */
#define IM_gm		0x0f
#define IM_FREQ		4	/* stop all outputs, maintain the frequency */
#define IM_WAITSW	7	/* stop all outputs, wait for software action */
#define IM_EDGETRIGFREQ	9	/* stop the output on the edge, maintain the frequency */
#define IM_LVLTRIGFREQ	10	/* stop the output at level, maintain the frequency */

#define IM_EXECWAIT	2
#define IM_EXECFAULT	3
#define IM_EXECDT	5
#define IM_WAIT		6
#define IM_JMPWAIT	1
#define IM_EDGETRIG	8

/* EVCTRLx.CFG (25.5.6) */
#define CFG_gm		0xc0
#define CFG_FILTERON	0x40

/* DBGCTRL (25.5.18) */
#define DBGRUN_bm	0x01
#define FAULTDET_bm	0x04

/* DLYCTRL (25.5.14) */
#define DLYSEL_gm	0x03
#define DLYSEL_INBLANK	1
#define DLYSEL_EVENT	2
#define DLYTRIG_gm	0x0c
#define DLYTRIG_gp	2
#define DLYPRESC_gm	0x30
#define DLYPRESC_gp	4
#define DLY_CMPASET	0
#define DLY_CMPACLR	1
#define DLY_CMPBSET	2
#define DLY_CMPBCLR	3

/* INTFLAGS / INTCTRL trigger bits */
#define TRIGA_bm	0x04
#define TRIGB_bm	0x08

/* STATUS */
#define PWMACTA_bm	0x40
#define PWMACTB_bm	0x80
#define ENRDY_bm	0x01
#define CMDRDY_bm	0x02

static inline uint8_t rd(avr_t *avr, avr_io_addr_t a) { return avr->data[a]; }

static int tcd_enabled(avr_tcd_t *p)
{
	return (rd(p->io.avr, p->r_ctrla) & ENABLE_bm) != 0;
}

static uint32_t reg12(avr_tcd_t *p, avr_io_addr_t a)
{
	avr_t *avr = p->io.avr;
	return (avr->data[a] | (avr->data[a + 1] << 8)) & 0x0fff;
}

static uint32_t tcd_top(avr_tcd_t *p)
{
	return reg12(p, p->r_cmpbclr);
}

static uint8_t tcd_mode(avr_tcd_t *p)
{
	return rd(p->io.avr, p->r_ctrlb) & WGMODE_gm;
}

static void tcd_delay_trigger(avr_tcd_t *p, int trig);

static void tcd_pulse(avr_tcd_t *p, int irq)
{
	avr_raise_irq(p->io.irq + irq, 1);
	avr_raise_irq(p->io.irq + irq, 0);
	switch (irq) {
	case AVR_TCD_IRQ_EVT_CMPASET: tcd_delay_trigger(p, DLY_CMPASET); break;
	case AVR_TCD_IRQ_EVT_CMPBSET: tcd_delay_trigger(p, DLY_CMPBSET); break;
	case AVR_TCD_IRQ_EVT_CMPBCLR: tcd_delay_trigger(p, DLY_CMPBCLR); break;
	}
}

/* WOC / WOD mirror WOA or WOB (CTRLC.CMPCSEL / CMPDSEL) when enabled by
 * FAULTCTRL.CMPCEN / CMPDEN (25.3.3.7). */
static void tcd_publish_cd(avr_tcd_t *p)
{
	uint8_t ctrlc = rd(p->io.avr, p->r_ctrlc);
	uint8_t fault = rd(p->io.avr, p->r_faultctrl);
	uint8_t c = (ctrlc & CMPCSEL_bm) ? p->wob_raw : p->woa_raw;
	uint8_t d = (ctrlc & CMPDSEL_bm) ? p->wob_raw : p->woa_raw;

	if ((fault & CMPCEN_bm) && c != p->woc) {
		p->woc = c;
		avr_raise_irq(p->io.irq + AVR_TCD_IRQ_WOC, c);
	}
	if ((fault & CMPDEN_bm) && d != p->wod) {
		p->wod = d;
		avr_raise_irq(p->io.irq + AVR_TCD_IRQ_WOD, d);
	}
	/* A disabled output is released (driven low) rather than left stuck high. */
	if (!(fault & CMPCEN_bm) && p->woc) {
		p->woc = 0;
		avr_raise_irq(p->io.irq + AVR_TCD_IRQ_WOC, 0);
	}
	if (!(fault & CMPDEN_bm) && p->wod) {
		p->wod = 0;
		avr_raise_irq(p->io.irq + AVR_TCD_IRQ_WOD, 0);
	}
}

/* Input-mode override (AVR Dx): a forced output takes the FAULTCTRL.CMPx state
 * (25.5.13). Modes 4-6 force both outputs while the input is active, mode 7 until
 * RESTART; 9 and 10 only the output of the triggering input. A blocked output
 * (modes 9, 10) is released once its on-time has ended; a held one (modes 1-3)
 * when the flow resumes. */
static uint8_t tcd_override(avr_tcd_t *p, int b, uint8_t nat)
{
	avr_t *avr = p->io.avr;
	uint8_t ma = rd(avr, p->base + TCDR_INPUTCTRLA) & IM_gm;
	uint8_t mb = rd(avr, p->base + TCDR_INPUTCTRLB) & IM_gm;
	uint8_t *blk = b ? &p->blk_b : &p->blk_a;
	uint8_t force;

	if (!nat)
		*blk = 0;
	force = p->stopped || *blk || (b ? p->hold_b : p->hold_a) ||
		(p->waiting && (p->wait_all || p->wait_input == b)) ||
		(p->act_a && ma >= IM_FREQ && ma <= IM_WAIT) ||
		(p->act_b && mb >= IM_FREQ && mb <= IM_WAIT) ||
		(b ? (mb == IM_LVLTRIGFREQ && p->act_b) : (ma == IM_LVLTRIGFREQ && p->act_a));
	return force ? (rd(avr, p->r_faultctrl) >> b) & 1 : nat;
}

static void tcd_publish(avr_tcd_t *p, uint8_t mask, uint8_t *cur, uint8_t next)
{
	if (p->ext) {
		int b = mask != CMPAEN_bm;
		if (b) p->wob_nat = next; else p->woa_nat = next;
		next = tcd_override(p, b, next);
	}
	if (mask == CMPAEN_bm)
		p->woa_raw = next;
	else
		p->wob_raw = next;
	if ((rd(p->io.avr, p->r_faultctrl) & mask) && *cur != next) {
		*cur = next;
		avr_raise_irq(p->io.irq + (mask == CMPAEN_bm ? AVR_TCD_IRQ_WOA : AVR_TCD_IRQ_WOB), next);
		/* STATUS.PWMACTx: set on every WOx toggle, cleared by software. */
		p->io.avr->data[p->r_status] |= (mask == CMPAEN_bm ? PWMACTA_bm : PWMACTB_bm);
	}
	tcd_publish_cd(p);
}

/* Publish the waveform-output levels for the current up-ramp state. */
static void tcd_emit_state(avr_tcd_t *p, int32_t count)
{
	uint8_t mode = tcd_mode(p);
	uint8_t woa = 0, wob = 0;

	switch (mode) {
	case WGMODE_ONERAMP: {
		uint32_t aset = reg12(p, p->r_cmpaset), aclr = reg12(p, p->r_cmpaclr);
		uint32_t bset = reg12(p, p->r_cmpbset), bclr = reg12(p, p->r_cmpbclr);
		woa = (count >= (int32_t)aset && count < (int32_t)aclr);
		wob = (count >= (int32_t)bset && count < (int32_t)bclr);
		if (count == (int32_t)aclr && aclr)
			tcd_delay_trigger(p, DLY_CMPACLR);
		break;
	}
	case WGMODE_TWORAMP:
		if (p->phase == 0) {
			uint32_t aset = reg12(p, p->r_cmpaset), aclr = reg12(p, p->r_cmpaclr);
			woa = (count >= (int32_t)aset && count < (int32_t)aclr);
		} else {
			uint32_t bset = reg12(p, p->r_cmpbset), bclr = reg12(p, p->r_cmpbclr);
			wob = (count >= (int32_t)bset && count < (int32_t)bclr);
		}
		break;
	case WGMODE_FOURRAMP:
		if (p->phase == 1)
			woa = 1;
		else if (p->phase == 3)
			wob = 1;
		break;
	default:
		break;
	}

	tcd_publish(p, CMPAEN_bm, &p->woa, woa);
	tcd_publish(p, CMPBEN_bm, &p->wob, wob);

	/* EVSYS generators: a pulse when the counter matches CMPASET / CMPBSET. */
	{
		uint32_t aset = reg12(p, p->r_cmpaset), bset = reg12(p, p->r_cmpbset);
		switch (mode) {
		case WGMODE_ONERAMP:
			if ((uint32_t)count == aset) tcd_pulse(p, AVR_TCD_IRQ_EVT_CMPASET);
			if ((uint32_t)count == bset) tcd_pulse(p, AVR_TCD_IRQ_EVT_CMPBSET);
			break;
		case WGMODE_TWORAMP:
			if (p->phase == 0 && (uint32_t)count == aset)
				tcd_pulse(p, AVR_TCD_IRQ_EVT_CMPASET);
			if (p->phase == 1 && (uint32_t)count == bset)
				tcd_pulse(p, AVR_TCD_IRQ_EVT_CMPBSET);
			break;
		case WGMODE_FOURRAMP:
			if (count == 0 && p->phase == 1) tcd_pulse(p, AVR_TCD_IRQ_EVT_CMPASET);
			if (count == 0 && p->phase == 3) tcd_pulse(p, AVR_TCD_IRQ_EVT_CMPBSET);
			break;
		default:
			break;
		}
	}
}

static void tcd_dualslope_action(avr_tcd_t *p, int32_t count)
{
	uint32_t aset = reg12(p, p->r_cmpaset);
	uint32_t bset = reg12(p, p->r_cmpbset);

	if (p->dir > 0) {
		if ((uint32_t)count == aset) tcd_pulse(p, AVR_TCD_IRQ_EVT_CMPASET);
		if ((uint32_t)count == bset) tcd_pulse(p, AVR_TCD_IRQ_EVT_CMPBSET);
	}
	if (p->dir < 0) {
		if ((uint32_t)count == bset)
			tcd_publish(p, CMPBEN_bm, &p->wob, 1);
		if ((uint32_t)count == aset)
			tcd_publish(p, CMPAEN_bm, &p->woa, 0);
	} else {
		if ((uint32_t)count == aset)
			tcd_publish(p, CMPAEN_bm, &p->woa, 1);
		if ((uint32_t)count == bset)
			tcd_publish(p, CMPBEN_bm, &p->wob, 0);
	}
}

static uint32_t tcd_phase_limit(avr_tcd_t *p)
{
	switch (tcd_mode(p)) {
	case WGMODE_ONERAMP:
	case WGMODE_DUALSLOPE:
		return tcd_top(p);
	case WGMODE_TWORAMP:
		return p->phase == 0 ? reg12(p, p->r_cmpaclr) : reg12(p, p->r_cmpbclr);
	case WGMODE_FOURRAMP:
		switch (p->phase) {
		case 0: return reg12(p, p->r_cmpaset);
		case 1: return reg12(p, p->r_cmpaclr);
		case 2: return reg12(p, p->r_cmpbset);
		default: return reg12(p, p->r_cmpbclr);
		}
	default:
		return tcd_top(p);
	}
}

static uint32_t tcd_next_delta(avr_tcd_t *p, int32_t count)
{
	uint32_t next = tcd_phase_limit(p) + 1u - (uint32_t)count;
	uint8_t mode = tcd_mode(p);

	if (mode == WGMODE_DUALSLOPE) {
		uint32_t top = tcd_top(p);
		if (p->dir < 0) {
			uint32_t aset = reg12(p, p->r_cmpaset);
			uint32_t bset = reg12(p, p->r_cmpbset);
			next = (uint32_t)count + 1u;
			if ((uint32_t)count > aset && (uint32_t)(count - (int32_t)aset) < next)
				next = (uint32_t)(count - (int32_t)aset);
			if ((uint32_t)count > bset && (uint32_t)(count - (int32_t)bset) < next)
				next = (uint32_t)(count - (int32_t)bset);
			return next;
		}

		next = top + 1u - (uint32_t)count;
		{
			uint32_t aset = reg12(p, p->r_cmpaset);
			uint32_t bset = reg12(p, p->r_cmpbset);
			if ((uint32_t)count < aset && aset - (uint32_t)count < next)
				next = aset - (uint32_t)count;
			if ((uint32_t)count < bset && bset - (uint32_t)count < next)
				next = bset - (uint32_t)count;
		}
		return next;
	}

	if (mode == WGMODE_ONERAMP) {
		uint32_t b[4] = {
			reg12(p, p->r_cmpaset),
			reg12(p, p->r_cmpaclr),
			reg12(p, p->r_cmpbset),
			reg12(p, p->r_cmpbclr),
		};
		for (int i = 0; i < 4; i++)
			if ((uint32_t)count < b[i] && b[i] - (uint32_t)count < next)
				next = b[i] - (uint32_t)count;
		return next;
	}

	if (mode == WGMODE_TWORAMP) {
		uint32_t cmp = p->phase == 0 ? reg12(p, p->r_cmpaset) : reg12(p, p->r_cmpbset);
		if ((uint32_t)count < cmp && cmp - (uint32_t)count < next)
			next = cmp - (uint32_t)count;
		return next;
	}

	return next;
}

/*
 * CLK_PER cycles per TCD count. The counter advances on CLK_TCD divided by
 * SYNCPRES (1/2/4/8) * CNTPRES (1/4/32) (DS40002205A 22.2.1, 22.5.1). simavr
 * schedules in CLK_PER cycles, so convert by the ratio CLK_PER / f_TCD, where
 * f_TCD is the CTRLA.CLKSEL source:
 *   - SYSCLK : f_TCD = CLK_PER (ratio 1, the common case)
 *   - OSC20M : the *unprescaled* 16/20 MHz oscillator, so a prescaled main
 *              clock makes the TCD run faster than the CPU
 *   - EXTCLK / reserved : no external-clock pin is modelled; track CLK_PER
 * Sub-CLK_PER resolution is not representable, so the result is rounded and
 * clamped to >= 1 cycle/count.
 */
static uint32_t tcd_prescale(avr_tcd_t *p)
{
	avr_t *avr = p->io.avr;
	uint8_t ctrla = rd(avr, p->r_ctrla);
	uint32_t sync = 1u << ((ctrla & SYNCPRES_gm) >> SYNCPRES_gp);
	static const uint8_t cnt[4] = { 1, 4, 32, 32 };
	uint32_t c = cnt[(ctrla & CNTPRES_gm) >> CNTPRES_gp];
	uint32_t div = sync * c;

	uint32_t clk_per = avr->frequency ? avr->frequency : 1;
	uint32_t f_tcd;
	uint8_t clksel = (ctrla & CLKSEL_gm) >> CLKSEL_gp;
	if (p->clk_fn && clksel != CLKSEL_SYSCLK) {
		f_tcd = p->clk_fn(p->clk_param, clksel);
		if (!f_tcd)
			f_tcd = clk_per;
		uint64_t cyc = ((uint64_t)div * clk_per + f_tcd / 2) / f_tcd;
		return cyc < 1 ? 1 : (uint32_t)cyc;
	}
	switch (clksel) {
	case CLKSEL_OSC20M:
		f_tcd = p->freq_osc20m ? p->freq_osc20m : clk_per;
		break;
	case CLKSEL_SYSCLK:
	default:	/* EXTCLK and reserved: unmodelled, track CLK_PER */
		f_tcd = clk_per;
		break;
	}

	uint64_t cyc = ((uint64_t)div * clk_per + f_tcd / 2) / f_tcd;
	return cyc < 1 ? 1 : (uint32_t)cyc;
}

/* Current counter value, for capture. */
static uint32_t tcd_count_now(avr_tcd_t *p)
{
	int64_t c;

	if (!tcd_enabled(p) || p->stopped || !p->prescale)
		return 0;
	if (p->waiting || p->dbg_halted == 2)
		return p->start_count < 0 ? 0 : (uint32_t)p->start_count;
	c = p->start_count +
		(int64_t)((p->io.avr->cycle - p->start_cycle) / p->prescale) * p->dir;
	return c < 0 ? 0 : (c > 0xfff ? 0xfff : (uint32_t)c);
}

/* Capture the counter into CAPTUREA / B. Held (blocked) until the high byte of
 * that register is read (25.5.19). */
static void tcd_capture(avr_tcd_t *p, int b)
{
	avr_t *avr = p->io.avr;
	uint8_t *lock = b ? &p->cap_lock_b : &p->cap_lock_a;
	avr_io_addr_t a = p->base + (b ? TCDR_CAPTUREBL : TCDR_CAPTUREAL);
	uint32_t v;

	if (*lock)
		return;
	v = tcd_count_now(p);
	avr->data[a] = v & 0xff;
	avr->data[a + 1] = v >> 8;
	*lock = 1;
}

static uint8_t
avr_tcd_capture_high_read(struct avr_t *avr, avr_io_addr_t addr, void *param)
{
	avr_tcd_t *p = (avr_tcd_t *)param;

	if (addr == p->base + TCDR_CAPTUREAL + 1)
		p->cap_lock_a = 0;
	else
		p->cap_lock_b = 0;
	return avr->data[addr];
}

/* CLK_PER cycles per CLK_TCD_SYNC period (the counter clock without CNTPRES). */
static uint32_t tcd_sync_cycles(avr_tcd_t *p)
{
	static const uint8_t cnt[4] = { 1, 4, 32, 32 };
	uint32_t c = cnt[(rd(p->io.avr, p->r_ctrla) & CNTPRES_gm) >> CNTPRES_gp];
	uint32_t n = tcd_prescale(p) / c;

	return n ? n : 1;
}

static avr_cycle_count_t
avr_tcd_blank_end(struct avr_t *avr, avr_cycle_count_t when, void *param)
{
	((avr_tcd_t *)param)->blanking = 0;
	return 0;
}

static avr_cycle_count_t
avr_tcd_progev(struct avr_t *avr, avr_cycle_count_t when, void *param)
{
	avr_tcd_t *p = (avr_tcd_t *)param;

	tcd_pulse(p, AVR_TCD_IRQ_EVT_PROGEV);
	return 0;
}

/* DLYCTRL.DLYTRIG compare match: open the input-A blanking window or start the
 * programmable output event, DLYPRESC * DLYVAL CLK_TCD_SYNC periods long
 * (25.3.3.4.1, 25.3.4.1). The two functions share the logic, so only one is
 * selected by DLYSEL. */
static void tcd_delay_trigger(avr_tcd_t *p, int trig)
{
	avr_t *avr = p->io.avr;
	uint8_t d = rd(avr, p->base + TCDR_DLYCTRL);
	avr_cycle_count_t n;

	if (!p->ext || ((d & DLYTRIG_gm) >> DLYTRIG_gp) != trig)
		return;
	n = (avr_cycle_count_t)(1u << ((d & DLYPRESC_gm) >> DLYPRESC_gp)) *
		rd(avr, p->base + TCDR_DLYVAL) * tcd_sync_cycles(p);

	switch (d & DLYSEL_gm) {
	case DLYSEL_INBLANK:
		if (!n)
			break;
		p->blanking = 1;
		avr_cycle_timer_register(avr, n, avr_tcd_blank_end, p);
		break;
	case DLYSEL_EVENT:
		if (!n)
			avr_tcd_progev(avr, avr->cycle, p);
		else
			avr_cycle_timer_register(avr, n, avr_tcd_progev, p);
		break;
	}
}

static void tcd_reapply(avr_tcd_t *p)
{
	tcd_publish(p, CMPAEN_bm, &p->woa, p->woa_nat);
	tcd_publish(p, CMPBEN_bm, &p->wob, p->wob_nat);
}

static avr_cycle_count_t
avr_tcd_tick(struct avr_t *avr, avr_cycle_count_t when, void *param);
static avr_cycle_count_t
avr_tcd_filter_a(struct avr_t *avr, avr_cycle_count_t when, void *param);
static avr_cycle_count_t
avr_tcd_filter_b(struct avr_t *avr, avr_cycle_count_t when, void *param);

/* ---- counter-flow input modes (25.3.3.4.5) ----
 * A TCD cycle is four segments: 0 DTA, 1 OTA, 2 DTB, 3 OTB. Input A acts on
 * segments 0-1 ("own" cycle), input B on 2-3; the opposite dead-time is the start
 * of the other input's cycle. */
static int tcd_segment(avr_tcd_t *p, uint32_t count)
{
	switch (tcd_mode(p)) {
	case WGMODE_ONERAMP:
		if (count < reg12(p, p->r_cmpaset)) return 0;
		if (count < reg12(p, p->r_cmpaclr)) return 1;
		if (count < reg12(p, p->r_cmpbset)) return 2;
		return 3;
	case WGMODE_TWORAMP:
		return p->phase == 0 ? (count < reg12(p, p->r_cmpaset) ? 0 : 1)
				     : (count < reg12(p, p->r_cmpbset) ? 2 : 3);
	case WGMODE_FOURRAMP:
		return p->phase & 3;
	default:
		return 0;
	}
}

static uint32_t tcd_seg_start(avr_tcd_t *p, int seg)
{
	if (tcd_mode(p) == WGMODE_ONERAMP) {
		switch (seg) {
		case 1: return reg12(p, p->r_cmpaset);
		case 2: return reg12(p, p->r_cmpaclr);
		case 3: return reg12(p, p->r_cmpbset);
		default: return 0;
		}
	}
	if (tcd_mode(p) == WGMODE_TWORAMP)
		return seg == 1 ? reg12(p, p->r_cmpaset) : seg == 3 ? reg12(p, p->r_cmpbset) : 0;
	return 0;
}

static int tcd_opp_dt(int cycle) { return cycle ? 0 : 2; }	/* DT of the other cycle */
static int tcd_own_dt(int cycle) { return cycle ? 2 : 0; }

/* Reposition the counter at the start of 'seg' and carry on running. */
static void tcd_goto(avr_tcd_t *p, int seg)
{
	avr_t *avr = p->io.avr;
	uint32_t count = tcd_seg_start(p, seg);

	p->waiting = 0;
	p->phase = tcd_mode(p) == WGMODE_TWORAMP ? seg >> 1 :
		   tcd_mode(p) == WGMODE_FOURRAMP ? seg : 0;
	p->dir = 1;
	p->prescale = tcd_prescale(p);
	p->start_cycle = avr->cycle;
	p->start_count = (int32_t)count;
	tcd_emit_state(p, (int32_t)count);
	p->dith_done = 0;
	p->jumped = p->in_tick;
	if (p->dbg_halted != 2)		/* during a freezing break only the position moves */
		avr_cycle_timer_register(avr,
			(avr_cycle_count_t)tcd_next_delta(p, (int32_t)count) * p->prescale,
			avr_tcd_tick, p);
}

/* Halt the counter where it is until input 'b' is released. */
static void tcd_halt(avr_tcd_t *p, int b, int all, int resume, int reposition)
{
	avr_cycle_timer_cancel(p->io.avr, avr_tcd_tick, p);
	if (reposition) {	/* "jump to the opposite dead-time and wait" */
		p->phase = tcd_mode(p) == WGMODE_TWORAMP ? resume >> 1 :
			   tcd_mode(p) == WGMODE_FOURRAMP ? resume : 0;
		p->start_count = (int32_t)tcd_seg_start(p, resume);
		tcd_emit_state(p, p->start_count);
	} else {
		p->start_count = (int32_t)tcd_count_now(p);
	}
	p->waiting = 1;
	p->wait_input = b;
	p->wait_all = all;
	p->resume_seg = resume;
	p->dith_done = 0;
	p->jumped = p->in_tick;
	tcd_reapply(p);
}

static void tcd_release(avr_tcd_t *p)
{
	p->arm_mode = 0;
	p->pend6 = 0;
	p->hold_a = p->hold_b = 0;
	tcd_goto(p, p->resume_seg);
}

/* A fault (active edge) on input 'b' in input mode 'mode'; 'changed' && !'active'
 * is the release. Dual Slope mode supports none of the flow modes (Table 25-5). */
static void tcd_flow_input(avr_tcd_t *p, int b, uint8_t mode, int changed, int active, int edge)
{
	int s, c;

	if (tcd_mode(p) == WGMODE_DUALSLOPE || p->stopped)
		return;
	if (changed && !active) {
		if (p->waiting && p->wait_input == b)
			tcd_release(p);
		else if (p->pend6 && p->wait_input == b)
			p->pend6 = 0;
	}
	if (!edge || p->waiting)
		return;
	s = tcd_segment(p, tcd_count_now(p));
	c = s >> 1;
	switch (mode) {
	case IM_JMPWAIT:
		if (c == b) {
			*(b ? &p->hold_b : &p->hold_a) = 1;
			tcd_halt(p, b, 0, tcd_opp_dt(b), 1);
		}
		break;
	case IM_EXECWAIT:
	case IM_EXECFAULT:
		if (c == b) {
			*(b ? &p->hold_b : &p->hold_a) = 1;
			p->arm_mode = mode;
			p->arm_input = b;
			tcd_goto(p, tcd_opp_dt(b));
		}
		break;
	case IM_EXECDT:
		if (s & 1)
			tcd_goto(p, tcd_opp_dt(c));
		break;
	case IM_WAIT:
		if (s & 1) {
			tcd_halt(p, b, 1, tcd_opp_dt(c), 1);
		} else {
			p->pend6 = 1;
			p->wait_input = b;
			p->resume_seg = tcd_opp_dt(c);
		}
		break;
	case IM_EDGETRIG:
		if ((s & 1) && c == b)
			tcd_goto(p, tcd_opp_dt(b));
		break;
	}
}

/* Called after the tick enters a segment: apply the mode 2 / 3 / 5 / 6 rules
 * that act at a segment boundary. */
static void tcd_flow_entry(avr_tcd_t *p, uint32_t count)
{
	uint8_t ma, mb;
	int s;

	if (!p->ext || tcd_mode(p) == WGMODE_DUALSLOPE)
		return;
	s = tcd_segment(p, count);
	if (count != tcd_seg_start(p, s))
		return;
	ma = rd(p->io.avr, p->base + TCDR_INPUTCTRLA) & IM_gm;
	mb = rd(p->io.avr, p->base + TCDR_INPUTCTRLB) & IM_gm;

	if ((s & 1) && p->pend6) {
		p->pend6 = 0;
		if (p->wait_input ? p->act_b : p->act_a) {
			tcd_halt(p, p->wait_input, 1, p->resume_seg, 0);
			return;
		}
	}
	if ((s & 1) && ((ma == IM_EXECDT && p->act_a) || (mb == IM_EXECDT && p->act_b))) {
		tcd_goto(p, tcd_opp_dt(s >> 1));	/* alternate dead-times while active */
		return;
	}
	if (p->arm_mode && s == tcd_own_dt(p->arm_input)) {
		int x = p->arm_input;

		if (x ? p->act_b : p->act_a) {
			if (p->arm_mode == IM_EXECFAULT)
				tcd_goto(p, tcd_opp_dt(x));
			else
				tcd_halt(p, x, 0, tcd_own_dt(x), 0);
		} else {
			p->arm_mode = 0;
			*(x ? &p->hold_b : &p->hold_a) = 0;
			tcd_reapply(p);
		}
	}
}

/* Dithering (25.3.3.5): a 4-bit accumulator takes DITHER at the end of each cycle;
 * on overflow the cycle is extended by the Table 25-7 number of counter clocks.
 * The extra clocks are added to the cycle length; where inside the cycle they land
 * is not modelled. */
static uint32_t tcd_dither_extra(avr_tcd_t *p)
{
	static const uint8_t add[3][4] = {	/* One / Two / Four Ramp x DITHERSEL */
		{ 1, 1, 0, 0 }, { 1, 2, 0, 0 }, { 1, 2, 1, 2 },
	};
	uint8_t mode = tcd_mode(p);
	uint16_t acc = p->dith_acc + (rd(p->io.avr, p->base + TCDR_DITVAL) & 0x0f);

	p->dith_acc = acc & 0x0f;
	if (mode == WGMODE_DUALSLOPE || acc < 16)
		return 0;
	return add[mode][rd(p->io.avr, p->base + TCDR_DITCTRL) & 3];
}

static avr_cycle_count_t
tcd_tick_core(struct avr_t *avr, avr_cycle_count_t when, void *param)
{
	avr_tcd_t *p = (avr_tcd_t *)param;
	uint8_t mode = tcd_mode(p);
	uint32_t top = tcd_top(p);
	int32_t count;

	if (!tcd_enabled(p))
		return 0;

	count = p->prescale ?
		p->start_count + (int32_t)(((when - p->start_cycle) / p->prescale) * p->dir) :
		(mode == WGMODE_DUALSLOPE ? -1 : (int32_t)tcd_phase_limit(p) + 1);

	if (mode == WGMODE_DUALSLOPE) {
		if (p->dir < 0 && count < 0) {
			p->start_cycle = when;
			p->start_count = 0;
			p->dir = 1;
			return when + (avr_cycle_count_t)tcd_next_delta(p, 0) * p->prescale;
		}
		if (p->dir > 0 && (uint32_t)count > top) {
			tcd_pulse(p, AVR_TCD_IRQ_EVT_CMPBCLR);
			avr_raise_interrupt(avr, &p->ovf);
			p->start_cycle = when;
			p->start_count = (int32_t)top;
			p->dir = -1;
			p->top = top;
			p->prescale = tcd_prescale(p);
			return when + (avr_cycle_count_t)tcd_next_delta(p, p->start_count) * p->prescale;
		}

		tcd_dualslope_action(p, count);
		p->start_cycle = when;
		p->start_count = count;
		return when + (avr_cycle_count_t)tcd_next_delta(p, count) * p->prescale;
	}

	if ((uint32_t)count > tcd_phase_limit(p)) {
		int last = mode == WGMODE_ONERAMP || (mode == WGMODE_TWORAMP && p->phase == 1) ||
			   (mode == WGMODE_FOURRAMP && p->phase == 3);
		if (p->ext && last && !p->dith_done) {
			uint32_t x = tcd_dither_extra(p);
			if (x) {
				p->dith_done = 1;
				return when + (avr_cycle_count_t)x * p->prescale;
			}
		}
		if (last)
			p->dith_done = 0;
		p->start_cycle = when;
		p->start_count = 0;

		if (mode == WGMODE_TWORAMP && p->phase == 0) {
			tcd_delay_trigger(p, DLY_CMPACLR);
			p->phase = 1;
			tcd_emit_state(p, 0);
			tcd_flow_entry(p, 0);
			return when + (avr_cycle_count_t)tcd_next_delta(p, 0) * p->prescale;
		}
		if (mode == WGMODE_FOURRAMP && p->phase < 3) {
			if (p->phase == 1)
				tcd_delay_trigger(p, DLY_CMPACLR);
			p->phase++;
			tcd_emit_state(p, 0);
			tcd_flow_entry(p, 0);
			return when + (avr_cycle_count_t)tcd_next_delta(p, 0) * p->prescale;
		}

		tcd_pulse(p, AVR_TCD_IRQ_EVT_CMPBCLR);
		avr_raise_interrupt(avr, &p->ovf);
		p->phase = 0;
		p->top = top;
		p->prescale = tcd_prescale(p);
		tcd_emit_state(p, 0);
		tcd_flow_entry(p, 0);
		return when + (avr_cycle_count_t)tcd_next_delta(p, 0) * p->prescale;
	}

	tcd_emit_state(p, count);
	p->start_cycle = when;
	p->start_count = count;
	tcd_flow_entry(p, (uint32_t)count);
	return when + (avr_cycle_count_t)tcd_next_delta(p, count) * p->prescale;
}

/* A flow change inside the tick re-registered the timer itself (or halted it). */
static avr_cycle_count_t
avr_tcd_tick(struct avr_t *avr, avr_cycle_count_t when, void *param)
{
	avr_tcd_t *p = (avr_tcd_t *)param;
	avr_cycle_count_t r;

	p->jumped = 0;
	p->in_tick = 1;
	r = tcd_tick_core(avr, when, param);
	p->in_tick = 0;
	if (p->jumped) {
		p->jumped = 0;
		return 0;
	}
	return r;
}

static void
avr_tcd_reschedule(avr_tcd_t *p)
{
	avr_t *avr = p->io.avr;

	avr_cycle_timer_cancel(avr, avr_tcd_tick, p);
	if (!tcd_enabled(p) || p->stopped || p->waiting || p->dbg_halted == 2)
		return;

	p->dith_done = 0;
	p->start_cycle = avr->cycle;
	p->top = tcd_top(p);
	p->prescale = tcd_prescale(p);
	p->phase = 0;
	if (tcd_mode(p) == WGMODE_DUALSLOPE) {
		p->dir = -1;
		p->start_count = (int32_t)p->top;
		tcd_publish(p, CMPAEN_bm, &p->woa, 0);
		tcd_publish(p, CMPBEN_bm, &p->wob, 0);
		avr_cycle_timer_register(avr,
			(avr_cycle_count_t)tcd_next_delta(p, p->start_count) * p->prescale,
			avr_tcd_tick, p);
		return;
	}

	p->dir = 1;
	p->start_count = 0;
	tcd_emit_state(p, 0);		/* outputs at count 0 (start of ramp) */
	avr_cycle_timer_register(avr,
			(avr_cycle_count_t)tcd_next_delta(p, 0) * p->prescale, avr_tcd_tick, p);
}

static void
avr_tcd_ctrla_write(struct avr_t *avr, avr_io_addr_t addr, uint8_t v, void *param)
{
	avr_tcd_t *p = (avr_tcd_t *)param;
	avr_core_watch_write(avr, addr, v);
	p->stopped = p->waiting = p->arm_mode = p->pend6 = p->hold_a = p->hold_b = 0;
	p->dith_acc = p->dith_done = 0;
	if (!(v & ENABLE_bm)) {
		avr_cycle_timer_cancel(avr, avr_tcd_filter_a, p);
		avr_cycle_timer_cancel(avr, avr_tcd_filter_b, p);
	}
	avr_tcd_reschedule(p);
}

/* CTRLB / FAULTCTRL / the compare registers: store, then re-evaluate the
 * waveform schedule if running (so duty / mode changes take effect). */
static void
avr_tcd_cfg_write(struct avr_t *avr, avr_io_addr_t addr, uint8_t v, void *param)
{
	avr_tcd_t *p = (avr_tcd_t *)param;
	avr_core_watch_write(avr, addr, v);
	if (tcd_enabled(p))
		avr_tcd_reschedule(p);
}

static void
avr_tcd_intflags_write(struct avr_t *avr, avr_io_addr_t addr, uint8_t v, void *param)
{
	avr_tcd_t *p = (avr_tcd_t *)param;
	uint8_t res = avr->data[addr] & ~v;	/* write-1-to-clear */
	avr_core_watch_write(avr, addr, res);
	if (!(res & OVF_bm))
		avr_clear_interrupt(avr, &p->ovf);
	if (p->trig.vector && !(res & (TRIGA_bm | TRIGB_bm)))
		avr_clear_interrupt(avr, &p->trig);
}

/* INTCTRL: enabling TRIGA/TRIGB while its flag is already set raises the TRIG
 * request; disabling every flagged source withdraws a pending one. */
static void
avr_tcd_intctrl_write(struct avr_t *avr, avr_io_addr_t addr, uint8_t v, void *param)
{
	avr_tcd_t *p = (avr_tcd_t *)param;
	uint8_t flags = rd(avr, p->r_intflags) & (TRIGA_bm | TRIGB_bm);

	avr_core_watch_write(avr, addr, v);
	if (!p->trig.vector)
		return;
	if (flags & v)
		avr_raise_interrupt(avr, &p->trig);
	else
		avr_clear_interrupt(avr, &p->trig);
}

/* CTRLC only changes which waveform WOC / WOD show: no reschedule. */
static void
avr_tcd_ctrlc_write(struct avr_t *avr, avr_io_addr_t addr, uint8_t v, void *param)
{
	avr_tcd_t *p = (avr_tcd_t *)param;
	avr_core_watch_write(avr, addr, v);
	tcd_publish_cd(p);
}

/* STATUS.PWMACTA/B are write-1-to-clear; the sync-ready bits read as 1. */
static void
avr_tcd_status_write(struct avr_t *avr, avr_io_addr_t addr, uint8_t v, void *param)
{
	(void)param;
	avr->data[addr] = (avr->data[addr] & ~(v & (PWMACTA_bm | PWMACTB_bm))) | ENRDY_bm | CMDRDY_bm;
}

/* The INPUTMODE action of a Fault change on input 'b' (shared with the debug-break
 * FAULTDET fault). */
static void tcd_input_fault(avr_tcd_t *p, int b, int changed, int active, int edge)
{
	uint8_t mode = rd(p->io.avr, p->base + (b ? TCDR_INPUTCTRLB : TCDR_INPUTCTRLA)) & IM_gm;
	uint8_t nat = b ? p->wob_nat : p->woa_nat;

	if (edge && mode == IM_EDGETRIGFREQ && nat)
		*(b ? &p->blk_b : &p->blk_a) = 1;
	if (changed && !active && mode == IM_LVLTRIGFREQ && nat)
		*(b ? &p->blk_b : &p->blk_a) = 1;	/* released mid on-time */
	if (edge && mode == IM_WAITSW) {
		p->stopped = 1;
		avr_cycle_timer_cancel(p->io.avr, avr_tcd_tick, p);
	}
	tcd_flow_input(p, b, mode, changed, active, edge);
	tcd_reapply(p);
}

/* Process input 'b' going to 'level' (after the digital filter, if enabled). An
 * active edge (EVCTRLx.EDGE: 1 = rising / high, 0 = falling / low) with TRIGEI set
 * flags TRIGA / TRIGB (25.5.9). On the Dx the event also captures the counter
 * (ACTION) and drives the INPUTMODE fault action; input A is ignored while its
 * blanking window is open (25.3.3.4.1). */
static void tcd_process_input(avr_tcd_t *p, int b, uint8_t level)
{
	avr_t *avr = p->io.avr;
	uint8_t *prev = b ? &p->in_b : &p->in_a;
	uint8_t ev = rd(avr, b ? p->r_evctrla + 1 : p->r_evctrla);
	uint8_t bit = b ? TRIGB_bm : TRIGA_bm;
	uint8_t active = level == !!(ev & EDGE_bm);
	int changed = level != *prev;
	int edge = changed && active;

	*prev = level;
	if (p->ext) {
		uint8_t *act = b ? &p->act_b : &p->act_a;

		if (p->dbg_halted == 2)
			return;		/* the peripheral ignores events during a break */
		if (!b && p->blanking)
			changed = edge = 0;
		if (!tcd_enabled(p) || !(ev & TRIGEI_bm)) {
			*act = 0;
			tcd_reapply(p);
			return;
		}
		if (changed)
			*act = active;
		if (edge && (ev & ACTION_bm))
			tcd_capture(p, b);
		tcd_input_fault(p, b, changed, active, edge);
	}
	if (!edge || !(ev & TRIGEI_bm) || !tcd_enabled(p))
		return;
	avr_core_watch_write(avr, p->r_intflags, rd(avr, p->r_intflags) | bit);
	if (p->trig.vector && (rd(avr, p->r_intctrl) & bit))
		avr_raise_interrupt(avr, &p->trig);
}

/* EVCTRLx.CFG = FILTERON: a change must hold for four counter clocks before it is
 * seen (25.3.3.4.2). ASYNCON and the default synchronous path act immediately; the
 * 2-3 CLK_TCD_SYNC synchroniser latency is not modelled. */
static avr_cycle_count_t
avr_tcd_filter_a(struct avr_t *avr, avr_cycle_count_t when, void *param)
{
	avr_tcd_t *p = (avr_tcd_t *)param;
	tcd_process_input(p, 0, p->filt_level[0]);
	return 0;
}

static avr_cycle_count_t
avr_tcd_filter_b(struct avr_t *avr, avr_cycle_count_t when, void *param)
{
	avr_tcd_t *p = (avr_tcd_t *)param;
	tcd_process_input(p, 1, p->filt_level[1]);
	return 0;
}

static void
avr_tcd_input_event(struct avr_irq_t *irq, uint32_t value, void *param)
{
	avr_tcd_t *p = (avr_tcd_t *)param;
	avr_t *avr = p->io.avr;
	int b = (irq->irq - p->base_irq) == AVR_TCD_IRQ_IN_B;
	uint8_t level = value & 1;

	if (p->ext && (rd(avr, b ? p->r_evctrla + 1 : p->r_evctrla) & CFG_gm) == CFG_FILTERON) {
		avr_cycle_timer_t fn = b ? avr_tcd_filter_b : avr_tcd_filter_a;

		p->filt_level[b] = level;
		if (level == (b ? p->in_b : p->in_a))
			avr_cycle_timer_cancel(avr, fn, p);	/* spike: back to the accepted level */
		else
			avr_cycle_timer_register(avr, 4 * (avr_cycle_count_t)tcd_prescale(p), fn, p);
		return;
	}
	tcd_process_input(p, b, level);
}

/* CTRLE strobes (Dx): SCAPTUREA/B capture the counter, RESTART restarts it (a
 * mode-7 stop is released unless an input is still active). The strobes read
 * back as 0. */
static void
avr_tcd_ctrle_write(struct avr_t *avr, avr_io_addr_t addr, uint8_t v, void *param)
{
	avr_tcd_t *p = (avr_tcd_t *)param;

	avr_core_watch_write(avr, addr, v & ~(RESTART_bm | SCAPTUREA_bm | SCAPTUREB_bm));
	if (v & SCAPTUREA_bm) tcd_capture(p, 0);
	if (v & SCAPTUREB_bm) tcd_capture(p, 1);
	if ((v & RESTART_bm) && tcd_enabled(p)) {
		uint8_t ma = rd(avr, p->base + TCDR_INPUTCTRLA) & IM_gm;
		uint8_t mb = rd(avr, p->base + TCDR_INPUTCTRLB) & IM_gm;

		p->stopped = (ma == IM_WAITSW && p->act_a) || (mb == IM_WAITSW && p->act_b);
		p->waiting = p->arm_mode = p->pend6 = p->hold_a = p->hold_b = 0;
		avr_tcd_reschedule(p);
		tcd_reapply(p);
	}
}

void
avr_tcd_debug_halt(avr_t * avr, avr_tcd_t * p, int halted)
{
	uint8_t dbg = rd(avr, p->base + TCDR_DBGCTRL);
	int frozen;

	if (!p->ext || !!halted == !!p->dbg_halted)
		return;
	if (halted && !tcd_enabled(p))
		return;
	if (halted) {
		p->dbg_halted = 1;
		if (!(dbg & DBGRUN_bm)) {	/* DBGRUN = 0: freeze the counter for the break */
			p->start_count = (int32_t)tcd_count_now(p);
			p->dbg_halted = 2;
			avr_cycle_timer_cancel(avr, avr_tcd_tick, p);
			avr_cycle_timer_cancel(avr, avr_tcd_filter_a, p);
			avr_cycle_timer_cancel(avr, avr_tcd_filter_b, p);
		}
		if (dbg & FAULTDET_bm) {
			/* A Fault on both input channels for as long as the break lasts. */
			p->act_a = p->act_b = 1;
			avr_core_watch_write(avr, p->r_intflags,
					rd(avr, p->r_intflags) | TRIGA_bm | TRIGB_bm);
			if (p->trig.vector && (rd(avr, p->r_intctrl) & (TRIGA_bm | TRIGB_bm)))
				avr_raise_interrupt(avr, &p->trig);
			for (int b = 0; b < 2; b++)
				tcd_input_fault(p, b, 1, 1, 1);
			tcd_reapply(p);
		}
		return;
	}
	frozen = p->dbg_halted == 2;
	p->dbg_halted = 0;
	for (int b = 0; b < 2; b++) {	/* back to the real input levels */
		uint8_t ev = rd(avr, b ? p->r_evctrla + 1 : p->r_evctrla);
		uint8_t now = (ev & TRIGEI_bm) && ((b ? p->in_b : p->in_a) == !!(ev & EDGE_bm));
		uint8_t was = b ? p->act_b : p->act_a;

		*(b ? &p->act_b : &p->act_a) = now;
		if (was && !now)
			tcd_flow_input(p, b,
				rd(avr, p->base + (b ? TCDR_INPUTCTRLB : TCDR_INPUTCTRLA)) & IM_gm,
				1, 0, 0);
	}
	if (frozen && tcd_enabled(p) && !p->waiting && !p->stopped) {	/* resume where the break stopped it */
		p->start_cycle = avr->cycle;
		p->prescale = tcd_prescale(p);
		avr_cycle_timer_register(avr,
			(avr_cycle_count_t)tcd_next_delta(p, p->start_count) * p->prescale,
			avr_tcd_tick, p);
	}
	tcd_reapply(p);
}

/* Changing EVCTRLx.CFG drops a change still being qualified by the filter. */
static void
avr_tcd_evctrl_write(struct avr_t *avr, avr_io_addr_t addr, uint8_t v, void *param)
{
	avr_tcd_t *p = (avr_tcd_t *)param;

	avr_core_watch_write(avr, addr, v);
	if ((v & CFG_gm) != CFG_FILTERON)
		avr_cycle_timer_cancel(avr, addr == p->r_evctrla ? avr_tcd_filter_a : avr_tcd_filter_b, p);
}

/* A new INPUTMODE changes how the live outputs are forced. */
static void
avr_tcd_inputctrl_write(struct avr_t *avr, avr_io_addr_t addr, uint8_t v, void *param)
{
	avr_tcd_t *p = (avr_tcd_t *)param;

	avr_core_watch_write(avr, addr, v);
	tcd_reapply(p);
}

void
avr_tcd_set_trig_vector(
		avr_t * avr,
		avr_tcd_t * p,
		uint8_t vec_trig)
{
	p->ext = 1;
	p->trig.vector = vec_trig;
	p->trig.enable.reg = p->r_intctrl;
	p->trig.enable.mask = TRIGA_bm | TRIGB_bm;
	/* No 'raised' regbit: raising a vector sets every bit of its mask, and TRIGA /
	 * TRIGB are flagged individually by avr_tcd_input_event(). */
	p->trig.raise_sticky = 1;
	avr_register_vector(avr, &p->trig);
	avr_register_io_write(avr, p->base + TCDR_CTRLE, avr_tcd_ctrle_write, p);
	avr_register_io_read(avr, p->base + TCDR_CAPTUREAL + 1, avr_tcd_capture_high_read, p);
	avr_register_io_read(avr, p->base + TCDR_CAPTUREBL + 1, avr_tcd_capture_high_read, p);
	avr_register_io_write(avr, p->r_evctrla, avr_tcd_evctrl_write, p);
	avr_register_io_write(avr, p->r_evctrla + 1, avr_tcd_evctrl_write, p);
	avr_register_io_write(avr, p->base + TCDR_INPUTCTRLA, avr_tcd_inputctrl_write, p);
	avr_register_io_write(avr, p->base + TCDR_INPUTCTRLB, avr_tcd_inputctrl_write, p);
}

/* STATUS always reports the double-buffered logic ready. */
static uint8_t
avr_tcd_status_read(struct avr_t *avr, avr_io_addr_t addr, void *param)
{
	(void)param;
	return ENRDY_bm | CMDRDY_bm | (avr->data[addr] & (PWMACTA_bm | PWMACTB_bm));
}

static void
avr_tcd_reset(avr_io_t *io)
{
	avr_tcd_t *p = (avr_tcd_t *)io;
	avr_cycle_timer_cancel(p->io.avr, avr_tcd_tick, p);
	p->start_cycle = 0;
	p->start_count = 0;
	p->prescale = 1;
	p->top = 0;
	p->phase = 0;
	p->dir = 1;
	p->woa = p->wob = p->woa_raw = p->wob_raw = p->woc = p->wod = 0;
	p->in_a = p->in_b = 0;
	p->woa_nat = p->wob_nat = p->act_a = p->act_b = p->blk_a = p->blk_b = 0;
	p->stopped = p->blanking = p->cap_lock_a = p->cap_lock_b = 0;
	p->waiting = p->wait_all = p->pend6 = p->arm_mode = p->hold_a = p->hold_b = 0;
	p->jumped = p->dith_acc = p->dith_done = p->dbg_halted = 0;
	avr_cycle_timer_cancel(p->io.avr, avr_tcd_filter_a, p);
	avr_cycle_timer_cancel(p->io.avr, avr_tcd_filter_b, p);
	avr_cycle_timer_cancel(p->io.avr, avr_tcd_blank_end, p);
	avr_cycle_timer_cancel(p->io.avr, avr_tcd_progev, p);
	p->io.avr->data[p->r_status] = ENRDY_bm | CMDRDY_bm;
}

static const char *irq_names[AVR_TCD_IRQ_COUNT] = {
	[AVR_TCD_IRQ_WOA] = ">tcd.woa",
	[AVR_TCD_IRQ_WOB] = ">tcd.wob",
	[AVR_TCD_IRQ_WOC] = ">tcd.woc",
	[AVR_TCD_IRQ_WOD] = ">tcd.wod",
	[AVR_TCD_IRQ_EVT_CMPBCLR] = ">tcd.cmpbclr",
	[AVR_TCD_IRQ_EVT_CMPASET] = ">tcd.cmpaset",
	[AVR_TCD_IRQ_EVT_CMPBSET] = ">tcd.cmpbset",
	[AVR_TCD_IRQ_IN_A] = "tcd.ina",
	[AVR_TCD_IRQ_IN_B] = "tcd.inb",
	[AVR_TCD_IRQ_EVT_PROGEV] = ">tcd.progev",
};

static avr_io_t _io = {
	.kind = "tcd",
	.reset = avr_tcd_reset,
	.irq_names = irq_names,
};

void
avr_tcd_init(
		avr_t * avr,
		avr_tcd_t * p,
		avr_io_addr_t base,
		uint8_t vec_ovf,
		uint8_t osccfg_fuse_index,
		char name)
{
	memset(p, 0, sizeof(*p));
	p->io = _io;
	p->name = name;
	p->base = base;

	/* Resolve the OSC20M base from the OSCCFG fuse (matches avr_clkctrl.c). */
	uint8_t osccfg = (osccfg_fuse_index == 0xff) ? 0 :
					 avr->fuse[osccfg_fuse_index];
	p->freq_osc20m = ((osccfg & FREQSEL_gm) == FREQSEL_16MHZ) ?
					 16000000 : 20000000;
	p->r_ctrla = base + TCDR_CTRLA;
	p->r_ctrlb = base + TCDR_CTRLB;
	p->r_intctrl = base + TCDR_INTCTRL;
	p->r_intflags = base + TCDR_INTFLAGS;
	p->r_status = base + TCDR_STATUS;
	p->r_faultctrl = base + TCDR_FAULTCTRL;
	p->r_ctrlc = base + TCDR_CTRLC;
	p->r_evctrla = base + TCDR_EVCTRLA;
	p->r_cmpaset = base + TCDR_CMPASETL;
	p->r_cmpaclr = base + TCDR_CMPACLRL;
	p->r_cmpbset = base + TCDR_CMPBSETL;
	p->r_cmpbclr = base + TCDR_CMPBCLRL;
	p->prescale = 1;

	/* OVF: enabled by INTCTRL.OVF(0), flagged in INTFLAGS.OVF(0). */
	p->ovf.vector = vec_ovf;
	p->ovf.enable.reg = p->r_intctrl;
	p->ovf.enable.bit = 0;
	p->ovf.enable.mask = 1;
	p->ovf.raised.reg = p->r_intflags;
	p->ovf.raised.bit = 0;
	p->ovf.raised.mask = 1;
	p->ovf.raise_sticky = 1;

	avr_register_io(avr, &p->io);
	avr_register_vector(avr, &p->ovf);

	avr_io_setirqs(&p->io, AVR_IOCTL_TCD_GETIRQ(name), AVR_TCD_IRQ_COUNT, NULL);
	p->base_irq = p->io.irq[0].irq;

	avr_register_io_write(avr, p->r_ctrla, avr_tcd_ctrla_write, p);
	avr_register_io_write(avr, p->r_intflags, avr_tcd_intflags_write, p);
	avr_register_io_read(avr, p->r_status, avr_tcd_status_read, p);
	avr_register_io_write(avr, p->r_status, avr_tcd_status_write, p);
	avr_register_io_write(avr, p->r_ctrlc, avr_tcd_ctrlc_write, p);
	avr_register_io_write(avr, p->r_intctrl, avr_tcd_intctrl_write, p);
	avr_irq_register_notify(p->io.irq + AVR_TCD_IRQ_IN_A, avr_tcd_input_event, p);
	avr_irq_register_notify(p->io.irq + AVR_TCD_IRQ_IN_B, avr_tcd_input_event, p);

	/* CTRLB (mode), FAULTCTRL (output enables) and the four compare registers
	 * all influence the waveform schedule: reschedule when they change. */
	avr_register_io_write(avr, p->r_ctrlb, avr_tcd_cfg_write, p);
	avr_register_io_write(avr, p->r_faultctrl, avr_tcd_cfg_write, p);
	avr_register_io_write(avr, p->r_cmpaset, avr_tcd_cfg_write, p);
	avr_register_io_write(avr, p->r_cmpaset + 1, avr_tcd_cfg_write, p);
	avr_register_io_write(avr, p->r_cmpaclr, avr_tcd_cfg_write, p);
	avr_register_io_write(avr, p->r_cmpaclr + 1, avr_tcd_cfg_write, p);
	avr_register_io_write(avr, p->r_cmpbset, avr_tcd_cfg_write, p);
	avr_register_io_write(avr, p->r_cmpbset + 1, avr_tcd_cfg_write, p);
	avr_register_io_write(avr, p->r_cmpbclr, avr_tcd_cfg_write, p);
	avr_register_io_write(avr, p->r_cmpbclr + 1, avr_tcd_cfg_write, p);
}
