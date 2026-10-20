/*
	avr_tca.c

	"Modern" AVR (AVRxt) 16-bit Timer/Counter type A (TCA). See avr_tca.h.

	Scheduling model
	----------------
	The counter position is a *phase*: the number of prescaled ticks into the
	current period. A period is L ticks: TOP+1 for the single-direction modes
	(NORMAL / FRQ / SINGLESLOPE, counting up or down) and 2*TOP for the
	dual-slope modes (BOTTOM..TOP..BOTTOM). CNT and the direction follow from
	the phase. Rather than stepping every cycle, a single simavr cycle timer is
	scheduled to the *next* interesting phase: a compare match, TOP (dual-slope
	OVF) or the end of the period (the wrap, where OVF is raised and the double
	buffers are transferred). CNT reads compute the live value from elapsed
	cycles. Split mode runs the same scheme twice, one 8-bit down-counter per
	half.

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
#include "avr_tca.h"
#include "sim_cycle_timers.h"

/* CTRLA */
#define ENABLE_bm	0x01
#define CLKSEL_gm	0x0e
#define CLKSEL_gp	1

/* CTRLB (normal-mode view): WGMODE[2:0], ALUPD bit 3, CMP0/1/2EN at bits 4/5/6
 * (DS40002205A 20.5.2; avr-libc TCA_SINGLE_CMP0EN_bm=0x10). In split mode the
 * low-byte enables are bits 0-2 and the high-byte enables bits 4-6. */
#define WGMODE_gm		0x07
#define ALUPD_bm		0x08
enum {
	WGMODE_NORMAL = 0, WGMODE_FRQ = 1, WGMODE_SINGLESLOPE = 3,
	WGMODE_DSTOP = 5, WGMODE_DSBOTH = 6, WGMODE_DSBOTTOM = 7,
};

/* CTRLD / CTRLE / CTRLF */
#define SPLITM_bm	0x01
#define LUPD_bm		0x02
#define DIR_bm		0x01
#define CMD_gm		0x0c
#define CMD_gp		2
enum { CMD_NONE = 0, CMD_UPDATE = 1, CMD_RESTART = 2, CMD_RESET = 3 };
#define BV_gm		0x0f	/* CTRLF: PERBV bit 0, CMPnBV bits 1-3 */

/* EVCTRL (DS40002205A 20.5.9): CNTEI bit 0, EVACT bits 3:1 (values 0-3 defined). */
#define CNTEI_bm	0x01
#define EVACT_gm	0x0e
#define EVACT_gp	1
enum {
	EVACT_POSEDGE = 0,	/* count on positive edge event */
	EVACT_ANYEDGE = 1,	/* count on any edge event */
	EVACT_HIGHLVL = 2,	/* count on prescaled clock while event line is 1 */
	EVACT_UPDOWN  = 3,	/* count on prescaled clock; event controls direction */
};

/* INTCTRL / INTFLAGS: OVF (LUNF) bit 0, HUNF bit 1 (split only), CMPn bits 4-6 */
#define OVF_bm		0x01
#define HUNF_bm		0x02
#define CMP0_bm		0x10
#define CMP1_bm		0x20
#define CMP2_bm		0x40

#define R_CTRLC		(p->base + TCAR_CTRLC)
#define R_CTRLD		(p->base + TCAR_CTRLD)
#define R_CTRLF		(p->base + TCAR_CTRLFCLR)

static const uint16_t clksel_div[8] = { 1, 2, 4, 8, 16, 64, 256, 1024 };
static const uint8_t cmp_flag[3] = { CMP0_bm, CMP1_bm, CMP2_bm };
static const uint8_t cmpen_bm[3] = { 0x10, 0x20, 0x40 };	/* CTRLB CMP0/1/2EN */

static uint16_t tca_rd16(avr_tca_t *p, avr_io_addr_t a)
{
	return p->io.avr->data[a] | (p->io.avr->data[a + 1] << 8);
}

static void tca_wr16(avr_tca_t *p, avr_io_addr_t a, uint16_t v)
{
	p->io.avr->data[a] = v & 0xff;
	p->io.avr->data[a + 1] = v >> 8;
}

static int tca_split(avr_tca_t *p)
{
	return p->io.avr->data[R_CTRLD] & SPLITM_bm;
}

static int tca_enabled(avr_tca_t *p)
{
	return (p->io.avr->data[p->r_ctrla] & ENABLE_bm) != 0;
}

static int tca_wgmode(avr_tca_t *p)
{
	return p->io.avr->data[p->r_ctrlb] & WGMODE_gm;
}

static int tca_is_ds(avr_tca_t *p)
{
	int wg = tca_wgmode(p);
	return wg == WGMODE_DSTOP || wg == WGMODE_DSBOTH || wg == WGMODE_DSBOTTOM;
}

/* A mode that produces a waveform (NORMAL and the reserved codes do not). */
static int tca_is_wg(avr_tca_t *p)
{
	int wg = tca_wgmode(p);
	return wg == WGMODE_FRQ || wg == WGMODE_SINGLESLOPE || tca_is_ds(p);
}

static uint32_t tca_cmp(avr_tca_t *p, int ch)
{
	return tca_rd16(p, p->r_cmp[ch]);
}

/* TOP: PER, or CMP0 in FRQ mode. */
static uint32_t tca_top(avr_tca_t *p)
{
	return tca_wgmode(p) == WGMODE_FRQ ? tca_cmp(p, 0) : tca_rd16(p, p->r_per);
}

/* Length of one period in prescaled ticks. */
static uint32_t tca_period(avr_tca_t *p)
{
	uint32_t top = tca_top(p);
	if (tca_is_ds(p))
		return top ? 2 * top : 1;
	return top + 1;
}

/* Counter value and direction at 'phase' ticks into the period. */
static uint32_t tca_phase_to_cnt(avr_tca_t *p, uint32_t phase, int down, int *dir)
{
	uint32_t top = tca_top(p);

	if (tca_is_ds(p)) {
		if (phase > top) {
			*dir = 1;
			return 2 * top - phase;
		}
		*dir = 0;
		return phase;
	}
	*dir = down;
	if (phase > top)
		phase = top;
	return down ? top - phase : phase;
}

static uint32_t tca_cnt_to_phase(avr_tca_t *p, uint32_t cnt, int down)
{
	uint32_t top = tca_top(p), len = tca_period(p);

	if (tca_is_ds(p)) {
		if (cnt > top)
			return len;	/* beyond TOP: wraps immediately */
		return (down && cnt) ? 2 * top - cnt : cnt;
	}
	if (down)
		return cnt > top ? 0 : top - cnt;
	return cnt > top ? len : cnt;
}

/* Current counter value (and direction) from the running clock or the registers. */
static uint32_t tca_pos(avr_tca_t *p, int *dir)
{
	avr_t *avr = p->io.avr;

	if (!p->clock_running) {
		*dir = p->dir_down;
		return tca_rd16(p, p->r_cnt);
	}
	uint32_t len = tca_period(p);
	uint32_t ticks = (uint32_t)((avr->cycle - p->start_cycle) / p->prescale);
	return tca_phase_to_cnt(p, ticks % len, p->dir_down, dir);
}

/* EVACT mode if event counting is enabled (CNTEI), else -1. */
static int tca_evact(avr_tca_t *p)
{
	uint8_t ev = p->io.avr->data[p->r_evctrl];
	if (!(ev & CNTEI_bm))
		return -1;
	return (ev & EVACT_gm) >> EVACT_gp;
}

/*
 * Whether the prescaled-clock scheduler should be running. The clock never
 * drives the counter in the edge-count modes; HIGHLVL gates it on the event
 * line; UPDOWN always runs it, the event line only chooses the direction.
 */
static int tca_clock_should_run(avr_tca_t *p)
{
	if (!tca_enabled(p))
		return 0;
	switch (tca_evact(p)) {
	case EVACT_POSEDGE:
	case EVACT_ANYEDGE:	return 0;
	case EVACT_HIGHLVL:	return p->ev_input;
	default:		return 1;	/* UPDOWN, or CNTEI clear: free-running clock */
	}
}

/* Direction a single-direction mode counts in right now. */
static int tca_want_down(avr_tca_t *p)
{
	return tca_evact(p) == EVACT_UPDOWN ? p->ev_input : p->dir_sw;
}

/*
 * Next phase strictly after 'ph' at which something happens, up to and
 * including the period end. 'down' is the single-direction modes' direction.
 */
static uint32_t tca_next_phase(avr_tca_t *p, uint32_t ph)
{
	uint32_t top = tca_top(p), best = tca_period(p);
	int ds = tca_is_ds(p), wg = tca_wgmode(p);
	uint32_t cand[7];
	int n = 0;

	for (int ch = 0; ch < 3; ch++) {
		uint32_t c = tca_cmp(p, ch);
		if (c > top)
			continue;
		if (ds) {
			cand[n++] = c;
			cand[n++] = 2 * top - c;
		} else {
			cand[n++] = p->dir_down ? top - c : c;
		}
	}
	if (ds && (wg == WGMODE_DSTOP || wg == WGMODE_DSBOTH))
		cand[n++] = top;	/* OVF at TOP */
	for (int i = 0; i < n; i++)
		if (cand[i] > ph && cand[i] < best)
			best = cand[i];
	return best;
}

/*
 * Waveform-generator output for channel ch at counter value cnt / direction.
 * Single-slope (DS40002205A 20.3.3.4.3): set at BOTTOM, cleared on the CMPn
 * match, so WOn is high while CNT < CMPn; CMPn == BOTTOM is a static low and
 * CMPn > TOP a static high. Dual-slope (20.3.3.4.4): set at BOTTOM, cleared on
 * the match counting up, set on the match counting down; CMPn == BOTTOM is a
 * static low, CMPn == TOP a static high. FRQ toggles on each match and keeps
 * its state in frq_lvl. Only a channel with CMPnEN set drives its output.
 * While the timer is disabled the output is CTRLC.CMPnOV (20.5.3).
 */
static uint8_t tca_wo_level(avr_tca_t *p, int ch, uint32_t cnt, int down)
{
	avr_t *avr = p->io.avr;
	uint8_t ctrlb = avr->data[p->r_ctrlb];
	uint32_t cmp, top;

	if (!(ctrlb & cmpen_bm[ch]))
		return 0;
	if (!tca_enabled(p))
		return (avr->data[R_CTRLC] >> ch) & 1;
	switch (tca_wgmode(p)) {
	case WGMODE_FRQ:
		return p->frq_lvl[ch];
	case WGMODE_SINGLESLOPE:
		cmp = tca_cmp(p, ch);
		top = tca_top(p);
		if (cmp == 0)
			return 0;
		return cmp > top ? 1 : (cnt < cmp);
	case WGMODE_DSTOP:
	case WGMODE_DSBOTH:
	case WGMODE_DSBOTTOM:
		cmp = tca_cmp(p, ch);
		top = tca_top(p);
		if (cmp == 0)
			return 0;
		if (cmp >= top)
			return 1;
		return down ? (cnt <= cmp) : (cnt < cmp);
	default:
		return 0;
	}
}

static void tca_publish(avr_tca_t *p, int idx, uint8_t lvl, uint8_t ovr)
{
	uint8_t pin = (ovr << 1) | lvl;

	if (lvl != p->wo_level[idx]) {
		p->wo_level[idx] = lvl;
		avr_raise_irq(p->io.irq + (idx < 3 ? AVR_TCA_IRQ_WO0 + idx : AVR_TCA_IRQ_WO3 + idx - 3), lvl);
	}
	if (pin != p->pin_state[idx]) {
		p->pin_state[idx] = pin;
		avr_raise_irq(p->io.irq + (idx < 3 ? AVR_TCA_IRQ_PIN0 + idx : AVR_TCA_IRQ_PIN3 + idx - 3), pin);
	}
}

static uint32_t tca_split_cnt(avr_tca_t *p, int h);
static avr_cycle_count_t
avr_tca_half_event(struct avr_t *avr, avr_cycle_count_t when, void *param);

/* Split-mode output level for channel ch (0-2) of half h. */
static uint8_t tca_split_level(avr_tca_t *p, int h, int ch)
{
	avr_t *avr = p->io.avr;
	uint8_t en = 1 << (h * 4 + ch);

	if (!(avr->data[p->r_ctrlb] & en))
		return 0;
	if (!tca_enabled(p))
		return (avr->data[R_CTRLC] & en) ? 1 : 0;
	uint32_t cmp = avr->data[p->base + 0x28 + 2 * ch + h];
	uint32_t top = avr->data[p->base + 0x26 + h];
	if (cmp == 0)
		return 0;
	if (cmp > top)
		return 1;
	return tca_split_cnt(p, h) < cmp;
}

/* Recompute every WOn level from the live counter and publish any transition. */
static void tca_emit_wo(avr_tca_t *p)
{
	avr_t *avr = p->io.avr;
	uint8_t ctrlb = avr->data[p->r_ctrlb];

	if (tca_split(p)) {
		for (int h = 0; h < 2; h++)
			for (int ch = 0; ch < 3; ch++)
				tca_publish(p, h * 3 + ch, tca_split_level(p, h, ch),
					    (ctrlb >> (h * 4 + ch)) & 1);
		return;
	}
	int down;
	uint32_t cnt = tca_pos(p, &down);
	for (int ch = 0; ch < 3; ch++) {
		/* The pin is overridden whenever CMPnEN is set in a waveform mode,
		 * whether or not the timer is running. */
		uint8_t ovr = (ctrlb & cmpen_bm[ch]) && tca_is_wg(p) ? 1 : 0;
		tca_publish(p, ch, tca_wo_level(p, ch, cnt, down), ovr);
	}
	for (int ch = 3; ch < 6; ch++)
		tca_publish(p, ch, 0, 0);
}

/* --- double buffering ------------------------------------------------- */

/* ALUPD: LUPD stays set until every enabled compare channel has a valid buffer. */
static void tca_alupd_eval(avr_tca_t *p)
{
	avr_t *avr = p->io.avr;
	uint8_t ctrlb = avr->data[p->r_ctrlb];
	uint8_t bv = avr->data[R_CTRLF];

	if (tca_split(p) || !(ctrlb & ALUPD_bm))
		return;
	int all = 1;
	for (int ch = 0; ch < 3; ch++)
		if ((ctrlb & cmpen_bm[ch]) && !(bv & (2 << ch)))
			all = 0;
	p->lupd = !all;
}

static void tca_set_bv(avr_tca_t *p, uint8_t bv)
{
	p->io.avr->data[R_CTRLF] = p->io.avr->data[p->base + TCAR_CTRLFSET] = bv & BV_gm;
	tca_alupd_eval(p);
}

/* UPDATE condition: transfer valid buffers to PER / CMPn unless LUPD blocks it. */
static void tca_update(avr_tca_t *p, int force)
{
	avr_t *avr = p->io.avr;
	uint8_t bv = avr->data[R_CTRLF];

	if (tca_split(p) || (!force && p->lupd))
		return;
	if (bv & 1)
		tca_wr16(p, p->r_per, tca_rd16(p, p->base + TCAR_PERBUFL));
	for (int ch = 0; ch < 3; ch++)
		if (bv & (2 << ch))
			tca_wr16(p, p->r_cmp[ch], tca_rd16(p, p->base + TCAR_CMP0BUFL + 2 * ch));
	tca_set_bv(p, 0);
	if (avr->data[p->r_ctrlb] & ALUPD_bm)
		p->lupd = 1;
}

/* --- normal (16-bit) mode engine --------------------------------------- */

static avr_cycle_count_t
avr_tca_event(struct avr_t *avr, avr_cycle_count_t when, void *param)
{
	avr_tca_t *p = (avr_tca_t *)param;
	uint32_t ph = p->ev_phase;
	int wg = tca_wgmode(p), ds = tca_is_ds(p), dir;

	if (!tca_enabled(p)) {
		p->clock_running = 0;
		return 0;
	}

	if (ph >= tca_period(p)) {
		/* End of period: TOP -> BOTTOM wrap (up), reload (down) or BOTTOM (dual
		 * slope). OVF is raised and the buffers are transferred. */
		p->start_cycle = when;
		ph = 0;
		if (!ds || wg == WGMODE_DSBOTTOM || wg == WGMODE_DSBOTH)
			avr_raise_interrupt(avr, &p->ovf);
		tca_update(p, 0);
	} else if (ds && ph == tca_top(p) && (wg == WGMODE_DSTOP || wg == WGMODE_DSBOTH)) {
		avr_raise_interrupt(avr, &p->ovf);
	}

	/* Compare matches at this count. A compare value of 0 matches at the wrap
	 * (the forward search only looks at later phases). */
	uint32_t cnt = tca_phase_to_cnt(p, ph, p->dir_down, &dir);
	for (int ch = 0; ch < 3; ch++) {
		if (tca_cmp(p, ch) != cnt)
			continue;
		avr_raise_interrupt(avr, &p->cmp[ch]);
		if (wg == WGMODE_FRQ)
			p->frq_lvl[ch] ^= 1;
	}

	p->ev_phase = tca_next_phase(p, ph);
	avr_cycle_count_t next = p->start_cycle +
				(avr_cycle_count_t)p->ev_phase * p->prescale;
	if (next <= when)
		next = when + 1;
	tca_emit_wo(p);
	return next;
}

/* Latch the live counter into the registers and cancel every timer. */
static void tca_stop(avr_tca_t *p)
{
	avr_t *avr = p->io.avr;

	if (p->clock_running) {
		int dir;
		uint32_t cnt = tca_pos(p, &dir);
		tca_wr16(p, p->r_cnt, cnt);
		p->dir_down = dir;
	}
	for (int h = 0; h < 2; h++) {
		if (p->half[h].running)
			avr->data[p->r_cnt + h] = tca_split_cnt(p, h);
		p->half[h].running = 0;
	}
	avr_cycle_timer_cancel(avr, avr_tca_event, p);
	for (int h = 0; h < 2; h++)
		avr_cycle_timer_cancel(avr, avr_tca_half_event, &p->half[h]);
	p->clock_running = 0;
}

/* --- split mode: two independent 8-bit down-counters -------------------- */

/* Live counter of half h (0 = low, 1 = high). */
static uint32_t tca_split_cnt(avr_tca_t *p, int h)
{
	avr_t *avr = p->io.avr;

	if (!p->half[h].running)
		return avr->data[p->r_cnt + h];
	uint32_t top = avr->data[p->r_per + h];
	uint32_t ticks = (uint32_t)((avr->cycle - p->half[h].start_cycle) / p->prescale);
	return top - ticks % (top + 1);
}

/* Next phase after 'ph' for half h: a compare match, BOTTOM (UNF) or the reload. */
static uint32_t tca_split_next(avr_tca_t *p, int h, uint32_t ph)
{
	avr_t *avr = p->io.avr;
	uint32_t top = avr->data[p->r_per + h], best = top + 1;

	if (top > ph)
		best = top;	/* BOTTOM */
	for (int ch = 0; ch < 3; ch++) {
		uint32_t c = avr->data[p->r_cmp[ch] + h];
		if (c <= top && top - c > ph && top - c < best)
			best = top - c;
		/* WOn goes high once CNT drops below CMPn (it is high while CNT < CMPn). */
		if (c >= 1 && c <= top && top - c + 1 > ph && top - c + 1 < best)
			best = top - c + 1;
	}
	return best;
}

static avr_cycle_count_t
avr_tca_half_event(struct avr_t *avr, avr_cycle_count_t when, void *param)
{
	struct avr_tca_half_t *hf = (struct avr_tca_half_t *)param;
	avr_tca_t *p = hf->p;
	int h = hf->h;
	uint32_t top = avr->data[p->r_per + h];
	uint32_t ph = hf->ev_phase;

	if (!tca_enabled(p) || !tca_split(p)) {
		hf->running = 0;
		return 0;
	}
	if (ph >= top + 1) {	/* reload from PER */
		hf->start_cycle = when;
		ph = 0;
	}
	uint32_t cnt = top - ph;
	if (cnt == 0)
		avr_raise_interrupt(avr, h ? &p->hunf : &p->ovf);
	if (h == 0)	/* there is no compare interrupt for the high byte timer */
		for (int ch = 0; ch < 3; ch++)
			if (avr->data[p->r_cmp[ch]] == cnt)
				avr_raise_interrupt(avr, &p->cmp[ch]);

	hf->ev_phase = tca_split_next(p, h, ph);
	avr_cycle_count_t next = hf->start_cycle +
				(avr_cycle_count_t)hf->ev_phase * p->prescale;
	if (next <= when)
		next = when + 1;
	tca_emit_wo(p);
	return next;
}

static void avr_tca_reschedule(avr_tca_t *p)
{
	avr_t *avr = p->io.avr;

	tca_stop(p);

	if (!tca_clock_should_run(p) && !(tca_split(p) && tca_enabled(p))) {
		/* Disabled, or counter is event-clocked / frozen: no timer. */
		tca_emit_wo(p);
		return;
	}

	uint8_t clksel = (avr->data[p->r_ctrla] & CLKSEL_gm) >> CLKSEL_gp;
	p->prescale = clksel_div[clksel & 7];

	if (tca_split(p)) {
		for (int h = 0; h < 2; h++) {
			struct avr_tca_half_t *hf = &p->half[h];
			uint32_t top = avr->data[p->r_per + h];
			uint32_t cnt = avr->data[p->r_cnt + h];
			if (cnt > top)
				cnt = top;
			uint32_t ph = top - cnt;
			hf->start_cycle = avr->cycle - (avr_cycle_count_t)ph * p->prescale;
			hf->ev_phase = tca_split_next(p, h, ph);
			avr_cycle_count_t abs = hf->start_cycle +
					(avr_cycle_count_t)hf->ev_phase * p->prescale;
			avr_cycle_timer_register(avr, abs > avr->cycle ? abs - avr->cycle : 1,
						 avr_tca_half_event, hf);
			hf->running = 1;
		}
		tca_emit_wo(p);
		return;
	}

	/* Keep CNT continuity from its current register value. */
	int down = tca_is_ds(p) ? p->dir_down : tca_want_down(p);
	p->dir_down = down;
	uint32_t cnt = tca_rd16(p, p->r_cnt);
	uint32_t ph = tca_cnt_to_phase(p, cnt, down);
	p->start_cycle = avr->cycle - (avr_cycle_count_t)ph * p->prescale;

	p->ev_phase = tca_next_phase(p, ph);
	avr_cycle_count_t abs = p->start_cycle +
				(avr_cycle_count_t)p->ev_phase * p->prescale;
	avr_cycle_count_t rel = (abs > avr->cycle) ? (abs - avr->cycle) : 1;
	avr_cycle_timer_register(avr, rel, avr_tca_event, p);
	p->clock_running = 1;
	tca_emit_wo(p);
}

/*
 * Advance the (register-held) counter by one count in event-count mode, with
 * the same compare/overflow semantics as the clock engine: OVF fires on the
 * TOP->BOTTOM wrap, CMPn when CNT reaches CMPn (CMPn == 0 matches at the wrap).
 */
static void
tca_event_tick(avr_tca_t *p)
{
	avr_t *avr = p->io.avr;
	uint32_t top = tca_top(p);
	uint32_t cnt = tca_rd16(p, p->r_cnt);

	if (p->dir_sw ? cnt == 0 : cnt >= top) {	/* wrap / reload */
		cnt = p->dir_sw ? top : 0;
		avr_raise_interrupt(avr, &p->ovf);
		tca_update(p, 0);
	} else {
		cnt += p->dir_sw ? -1 : 1;
	}
	for (int ch = 0; ch < 3; ch++) {
		if (tca_cmp(p, ch) != cnt)
			continue;
		avr_raise_interrupt(avr, &p->cmp[ch]);
		if (tca_wgmode(p) == WGMODE_FRQ)
			p->frq_lvl[ch] ^= 1;
	}
	tca_wr16(p, p->r_cnt, cnt);
	tca_emit_wo(p);
}

/*
 * Event-line input (EVSYS routes the TCA0 user here). In the edge-count modes
 * a qualifying edge ticks the counter; in the level modes the line gates /
 * directs the prescaled clock, so re-evaluate the schedule. Not used in split mode.
 */
static void
avr_tca_ev_input(struct avr_irq_t *irq, uint32_t value, void *param)
{
	avr_tca_t *p = (avr_tca_t *)param;
	uint8_t level = value & 1;
	uint8_t prev = p->ev_input;

	if (level == prev)
		return;
	p->ev_input = level;

	if (!tca_enabled(p) || tca_split(p))
		return;

	switch (tca_evact(p)) {
	case EVACT_POSEDGE:
		if (!prev && level)
			tca_event_tick(p);
		break;
	case EVACT_ANYEDGE:
		tca_event_tick(p);
		break;
	case EVACT_HIGHLVL:
	case EVACT_UPDOWN:
		avr_tca_reschedule(p);	/* level gates / directs the clock */
		break;
	default:
		break;			/* CNTEI clear: event is ignored */
	}
}

/*
 * Register writes that change the schedule: latch the live counter first (under
 * the old configuration), store, then re-anchor.
 */
static void
avr_tca_sched_write(struct avr_t *avr, avr_io_addr_t addr,
					uint8_t v, void *param)
{
	avr_tca_t *p = (avr_tca_t *)param;
	int was_enabled = tca_enabled(p);

	tca_stop(p);
	avr_core_watch_write(avr, addr, v);
	if (addr == p->r_ctrla && !was_enabled && tca_enabled(p)) {
		/* FRQ toggles from the value CMPnOV held while stopped. */
		for (int ch = 0; ch < 3; ch++)
			p->frq_lvl[ch] = (avr->data[R_CTRLC] >> ch) & 1;
	}
	if (addr == p->r_ctrlb)
		tca_alupd_eval(p);
	avr_tca_reschedule(p);
}

static void
avr_tca_ctrlc_write(struct avr_t *avr, avr_io_addr_t addr,
					uint8_t v, void *param)
{
	avr_tca_t *p = (avr_tca_t *)param;
	avr_core_watch_write(avr, addr, v);
	if (!tca_enabled(p))
		for (int ch = 0; ch < 3; ch++)
			p->frq_lvl[ch] = (v >> ch) & 1;
	tca_emit_wo(p);
}

/* PER / CMP0-2: changing the period or a compare value moves the next event,
 * so re-evaluate the schedule if the timer is running (cf. avr_tcd_cfg_write). */
static void
avr_tca_cfg_write(struct avr_t *avr, avr_io_addr_t addr,
				  uint8_t v, void *param)
{
	avr_tca_t *p = (avr_tca_t *)param;

	if (!tca_enabled(p)) {
		avr_core_watch_write(avr, addr, v);	/* low or high byte */
		tca_emit_wo(p);
		return;
	}
	avr_tca_sched_write(avr, addr, v, param);
}

/* PERBUF / CMPnBUF: store and mark the buffer valid (normal mode only). */
static void
avr_tca_buf_write(struct avr_t *avr, avr_io_addr_t addr,
				  uint8_t v, void *param)
{
	avr_tca_t *p = (avr_tca_t *)param;
	avr_core_watch_write(avr, addr, v);
	if (tca_split(p))
		return;	/* buffers are unused in split mode */
	int off = addr - p->base;
	int bit = off < TCAR_CMP0BUFL ? 0 : 1 + (off - TCAR_CMP0BUFL) / 2;
	tca_set_bv(p, avr->data[R_CTRLF] | (1 << bit));
}

static void
avr_tca_ctrlf_write(struct avr_t *avr, avr_io_addr_t addr,
					uint8_t v, void *param)
{
	avr_tca_t *p = (avr_tca_t *)param;
	uint8_t bv = avr->data[R_CTRLF];

	tca_set_bv(p, addr == p->base + TCAR_CTRLFSET ? bv | v : bv & ~v);
}

/* RESET: every register of the block returns to its initial value. */
static void tca_hard_reset(avr_tca_t *p)
{
	avr_t *avr = p->io.avr;

	memset(avr->data + p->base, 0, 0x40);
	tca_wr16(p, p->r_per, 0xffff);
	tca_wr16(p, p->base + TCAR_PERBUFL, 0xffff);
	p->lupd = p->dir_sw = p->dir_down = 0;
	memset(p->frq_lvl, 0, sizeof(p->frq_lvl));
	avr_clear_interrupt(avr, &p->ovf);
	avr_clear_interrupt(avr, &p->hunf);
	for (int ch = 0; ch < 3; ch++)
		avr_clear_interrupt(avr, &p->cmp[ch]);
}

/*
 * CTRLECLR / CTRLESET (DS40002205A 20.5.5/6, 20.7.5/6). Normal mode: LUPD and
 * DIR set/clear individually, CMD (UPDATE / RESTART / RESET) is issued by a
 * write to CTRLESET and always reads 0. Split mode: CMD together with CMDEN
 * (both halves, 0x3) issues RESTART / RESET.
 */
static void
avr_tca_ctrle_write(struct avr_t *avr, avr_io_addr_t addr,
					uint8_t v, void *param)
{
	avr_tca_t *p = (avr_tca_t *)param;
	int set = addr == p->base + TCAR_CTRLESET;
	int cmd = (v & CMD_gm) >> CMD_gp;

	tca_stop(p);
	if (tca_split(p)) {
		if (!set || (v & 3) != 3)
			cmd = CMD_NONE;
		if (cmd == CMD_RESTART) {	/* both timers restart from TOP */
			avr->data[p->r_cnt] = avr->data[p->r_per];
			avr->data[p->r_cnt + 1] = avr->data[p->r_per + 1];
		}
		if (cmd == CMD_RESET && !tca_enabled(p)) {
			tca_hard_reset(p);
		}
		avr_tca_reschedule(p);
		return;
	}
	if (v & LUPD_bm)
		p->lupd = set;
	if (v & DIR_bm)
		p->dir_sw = set;
	if ((v & DIR_bm) && tca_evact(p) != EVACT_UPDOWN)
		p->dir_down = p->dir_sw;	/* UPDOWN: the event line owns the direction */
	if (set) {
		switch (cmd) {
		case CMD_UPDATE:
			tca_update(p, 1);
			break;
		case CMD_RESTART:
			tca_wr16(p, p->r_cnt, 0);
			p->dir_sw = p->dir_down = 0;
			memset(p->frq_lvl, 0, sizeof(p->frq_lvl));
			break;
		case CMD_RESET:
			if (!tca_enabled(p))
				tca_hard_reset(p);
			break;
		}
	}
	avr_tca_reschedule(p);
}

static uint8_t
avr_tca_ctrle_read(struct avr_t *avr, avr_io_addr_t addr, void *param)
{
	avr_tca_t *p = (avr_tca_t *)param;
	int dir;

	if (tca_split(p))
		return 0;
	tca_pos(p, &dir);
	return (p->lupd ? LUPD_bm : 0) | (dir ? DIR_bm : 0);
}

static uint8_t
avr_tca_cnt_read(struct avr_t *avr, avr_io_addr_t addr, void *param)
{
	avr_tca_t *p = (avr_tca_t *)param;

	if (tca_split(p)) {
		int h = addr - p->r_cnt;
		avr->data[p->r_cnt + h] = tca_split_cnt(p, h);
		return avr->data[addr];
	}
	if (addr != p->r_cnt)
		return avr->data[addr];		/* latched high byte */
	int dir;
	uint16_t cnt = tca_pos(p, &dir);
	tca_wr16(p, p->r_cnt, cnt);		/* latches the high byte */
	return cnt & 0xff;
}

static void
avr_tca_intflags_write(struct avr_t *avr, avr_io_addr_t addr,
					   uint8_t v, void *param)
{
	avr_tca_t *p = (avr_tca_t *)param;
	uint8_t res = avr->data[p->r_intflags] & ~v;	/* write-1-to-clear */
	avr_core_watch_write(avr, p->r_intflags, res);
	if (!(res & OVF_bm))
		avr_clear_interrupt(avr, &p->ovf);
	if (!(res & HUNF_bm))
		avr_clear_interrupt(avr, &p->hunf);
	for (int ch = 0; ch < 3; ch++)
		if (!(res & cmp_flag[ch]))
			avr_clear_interrupt(avr, &p->cmp[ch]);
}

static void
avr_tca_reset(avr_io_t *io)
{
	avr_tca_t *p = (avr_tca_t *)io;
	avr_t *avr = p->io.avr;
	avr_cycle_timer_cancel(avr, avr_tca_event, p);
	for (int h = 0; h < 2; h++) {
		avr_cycle_timer_cancel(avr, avr_tca_half_event, &p->half[h]);
		p->half[h].running = 0;
		p->half[h].start_cycle = 0;
		p->half[h].ev_phase = 0;
	}
	p->start_cycle = 0;
	p->prescale = 1;
	p->ev_phase = 0;
	p->clock_running = 0;
	p->ev_input = 0;
	p->lupd = p->dir_sw = p->dir_down = 0;
	memset(p->frq_lvl, 0, sizeof(p->frq_lvl));
	memset(p->wo_level, 0, sizeof(p->wo_level));
	memset(p->pin_state, 0, sizeof(p->pin_state));
	/* PER and PERBUF reset to 0xFFFF (DS40002205A 20.5.15), not 0 — the data[]
	 * clear loop already zeroed them, so restore the real TOP default here.
	 * CMPn/CMPnBUF reset to 0, which the clear loop already produced. */
	tca_wr16(p, p->r_per, 0xffff);
	tca_wr16(p, p->base + TCAR_PERBUFL, 0xffff);
}

static const char *irq_names[AVR_TCA_IRQ_COUNT] = {
	[AVR_TCA_IRQ_WO0] = ">tca.wo0",
	[AVR_TCA_IRQ_WO1] = ">tca.wo1",
	[AVR_TCA_IRQ_WO2] = ">tca.wo2",
	[AVR_TCA_IRQ_EV_IN] = "<tca.ev_in",
	[AVR_TCA_IRQ_PIN0] = ">tca.pin0",
	[AVR_TCA_IRQ_PIN1] = ">tca.pin1",
	[AVR_TCA_IRQ_PIN2] = ">tca.pin2",
	[AVR_TCA_IRQ_WO3] = ">tca.wo3",
	[AVR_TCA_IRQ_WO4] = ">tca.wo4",
	[AVR_TCA_IRQ_WO5] = ">tca.wo5",
	[AVR_TCA_IRQ_PIN3] = ">tca.pin3",
	[AVR_TCA_IRQ_PIN4] = ">tca.pin4",
	[AVR_TCA_IRQ_PIN5] = ">tca.pin5",
};

static avr_io_t _io = {
	.kind = "tca",
	.reset = avr_tca_reset,
	.irq_names = irq_names,
};

static void
tca_vector(avr_int_vector_t *v, uint8_t vnum, avr_io_addr_t reg, uint8_t bit)
{
	v->vector = vnum;
	v->enable.reg = reg;	/* INTCTRL bit */
	v->enable.bit = bit;
	v->enable.mask = 1;
	v->raised.reg = reg + 1;	/* INTFLAGS is INTCTRL + 1 */
	v->raised.bit = bit;
	v->raised.mask = 1;
	v->raise_sticky = 1;
}

void
avr_tca_init(
		avr_t * avr,
		avr_tca_t * p,
		avr_io_addr_t base,
		uint8_t vec_ovf,
		uint8_t vec_cmp0,
		uint8_t vec_cmp1,
		uint8_t vec_cmp2,
		char name)
{
	memset(p, 0, sizeof(*p));
	p->io = _io;
	p->name = name;
	p->base = base;
	p->r_ctrla = base + TCAR_CTRLA;
	p->r_ctrlb = base + TCAR_CTRLB;
	p->r_evctrl = base + TCAR_EVCTRL;
	p->r_intctrl = base + TCAR_INTCTRL;
	p->r_intflags = base + TCAR_INTFLAGS;
	p->r_cnt = base + TCAR_CNTL;
	p->r_per = base + TCAR_PERL;
	p->r_cmp[0] = base + TCAR_CMP0L;
	p->r_cmp[1] = base + TCAR_CMP1L;
	p->r_cmp[2] = base + TCAR_CMP2L;
	p->prescale = 1;
	for (int h = 0; h < 2; h++) {
		p->half[h].p = p;
		p->half[h].h = h;
	}

	/* INTCTRL/INTFLAGS share bit positions: OVF/LUNF=0, HUNF=1 (split only),
	 * CMP0=4, CMP1=5, CMP2=6. The split-mode HUNF vector follows OVF in every
	 * device's vector table. */
	tca_vector(&p->ovf, vec_ovf, p->r_intctrl, 0);
	tca_vector(&p->hunf, vec_ovf + 1, p->r_intctrl, 1);
	tca_vector(&p->cmp[0], vec_cmp0, p->r_intctrl, 4);
	tca_vector(&p->cmp[1], vec_cmp1, p->r_intctrl, 5);
	tca_vector(&p->cmp[2], vec_cmp2, p->r_intctrl, 6);

	avr_register_io(avr, &p->io);
	avr_io_setirqs(&p->io, AVR_IOCTL_TCA_GETIRQ(name), AVR_TCA_IRQ_COUNT, NULL);
	p->base_irq = p->io.irq[0].irq;
	avr_register_vector(avr, &p->ovf);
	avr_register_vector(avr, &p->hunf);
	avr_register_vector(avr, &p->cmp[0]);
	avr_register_vector(avr, &p->cmp[1]);
	avr_register_vector(avr, &p->cmp[2]);

	avr_register_io_write(avr, p->r_ctrla, avr_tca_sched_write, p);
	avr_register_io_write(avr, p->r_ctrlb, avr_tca_sched_write, p);
	avr_register_io_write(avr, base + TCAR_CTRLC, avr_tca_ctrlc_write, p);
	avr_register_io_write(avr, base + TCAR_CTRLD, avr_tca_sched_write, p);
	avr_register_io_write(avr, base + TCAR_CTRLECLR, avr_tca_ctrle_write, p);
	avr_register_io_write(avr, base + TCAR_CTRLESET, avr_tca_ctrle_write, p);
	avr_register_io_read(avr, base + TCAR_CTRLECLR, avr_tca_ctrle_read, p);
	avr_register_io_read(avr, base + TCAR_CTRLESET, avr_tca_ctrle_read, p);
	avr_register_io_write(avr, base + TCAR_CTRLFCLR, avr_tca_ctrlf_write, p);
	avr_register_io_write(avr, base + TCAR_CTRLFSET, avr_tca_ctrlf_write, p);
	avr_register_io_write(avr, p->r_evctrl, avr_tca_sched_write, p);
	avr_register_io_write(avr, p->r_intflags, avr_tca_intflags_write, p);
	avr_irq_register_notify(p->io.irq + AVR_TCA_IRQ_EV_IN, avr_tca_ev_input, p);
	avr_register_io_read(avr, p->r_cnt, avr_tca_cnt_read, p);
	avr_register_io_read(avr, p->r_cnt + 1, avr_tca_cnt_read, p);
	avr_register_io_write(avr, p->r_cnt, avr_tca_sched_write, p);
	avr_register_io_write(avr, p->r_cnt + 1, avr_tca_sched_write, p);

	/* PER and the three compare registers (both bytes) move scheduled events. */
	avr_register_io_write(avr, p->r_per, avr_tca_cfg_write, p);
	avr_register_io_write(avr, p->r_per + 1, avr_tca_cfg_write, p);
	for (int ch = 0; ch < 3; ch++) {
		avr_register_io_write(avr, p->r_cmp[ch], avr_tca_cfg_write, p);
		avr_register_io_write(avr, p->r_cmp[ch] + 1, avr_tca_cfg_write, p);
	}
	/* Double-buffer registers: PERBUF, CMP0-2BUF (both bytes each). */
	for (int off = TCAR_PERBUFL; off <= TCAR_CMP2BUFL + 1; off++)
		avr_register_io_write(avr, base + off, avr_tca_buf_write, p);
}
