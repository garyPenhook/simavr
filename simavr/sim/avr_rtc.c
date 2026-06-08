/*
	avr_rtc.c

	"Modern" AVR (AVRxt) Real-Time Counter (RTC) + Periodic Interrupt Timer
	(PIT). See avr_rtc.h.

	Scheduling model
	----------------
	The RTC clock (CLKSEL: internal 32.768 kHz / 1.024 kHz, 32 kHz crystal, or
	external — all modelled as their nominal frequency) is decoupled from
	CLK_PER, so a period of N RTC-clock ticks is N * (avr->frequency / f_rtc)
	CPU cycles. The conversion is captured when each function (re)starts.

	  * RTC counter: a prescaled up-counter 0..PER. Rather than stepping every
	    tick, one cycle timer is scheduled to the next interesting count — the
	    compare value (if 1..PER and above the current count) or the wrap point
	    (PER+1). On expiry the matching INTFLAGS bit (CMP and/or OVF) is set and
	    the single RTC_CNT vector raised if that source is enabled in INTCTRL.
	  * PIT: a free-running periodic source firing every 2^n RTC-clock ticks
	    (PITCTRLA.PERIOD), on its own RTC_PIT vector, independent of the RTC
	    prescaler.

	Synchronisation-busy: writing CTRLA/CNT/PER/CMP (or PITCTRLA) asserts the
	matching STATUS/PITSTATUS busy bit for the documented two-RTC-clock-cycle
	synchronisation latency (DS40002205A 23.12.2, 23.5.1), converted to CPU
	cycles, so a `while (RTC.STATUS & RTC_CTRLABUSY_bm)` poll actually spins
	until sync completes. The write itself still takes effect immediately in the
	model (write-during-busy is not blocked).

	Deliberate simplifications: a CLK_PER change after the RTC starts is not
	retro-applied until the function is reconfigured; CRYSTERR / external-clock
	pins are not modelled.

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
#include "avr_rtc.h"
#include "sim_cycle_timers.h"

/* CTRLA */
#define RTCEN_bm	0x01
#define PRESCALER_gm	0x78
#define PRESCALER_gp	3

/* STATUS busy bits (index == bit position). */
#define CTRLABUSY_bm	0x01
#define CNTBUSY_bm	0x02
#define PERBUSY_bm	0x04
#define CMPBUSY_bm	0x08
enum { BUSY_CTRLA = 0, BUSY_CNT = 1, BUSY_PER = 2, BUSY_CMP = 3 };

/* PITSTATUS */
#define PIT_CTRLBUSY_bm	0x01

/* INTCTRL / INTFLAGS (RTC counter) */
#define OVF_bm		0x01
#define CMP_bm		0x02

/* PITCTRLA */
#define PITEN_bm	0x01
#define PERIOD_gm	0x78
#define PERIOD_gp	3

/* PITINTCTRL / PITINTFLAGS */
#define PI_bm		0x01

static inline uint8_t rd(avr_t *avr, avr_io_addr_t a) { return avr->data[a]; }
static void set_bits(avr_t *avr, avr_io_addr_t a, uint8_t m)
{
	avr_core_watch_write(avr, a, avr->data[a] | m);
}

/* RTC clock source frequency (Hz). */
static uint32_t rtc_src_hz(avr_rtc_t *p)
{
	/* CLKSEL: 0=INT32K, 1=INT1K, 2=TOSC32K, 3=EXTCLK. */
	if ((rd(p->io.avr, p->r_clksel) & 0x03) == 1)
		return 1024;
	return 32768;
}

/* CPU cycles for the two-RTC-clock-cycle register synchronisation latency. */
static avr_cycle_count_t rtc_sync_cpu(avr_rtc_t *p)
{
	avr_t *avr = p->io.avr;
	uint64_t c = 2u * (uint64_t)avr->frequency / rtc_src_hz(p);
	return c ? (avr_cycle_count_t)c : 1;
}

/* Mark a STATUS busy bit asserted until the synchronisation completes. */
static void rtc_mark_busy(avr_rtc_t *p, int idx)
{
	p->busy_until[idx] = p->io.avr->cycle + rtc_sync_cpu(p);
}

static uint32_t rtc_prescale_div(avr_rtc_t *p)
{
	uint8_t v = (rd(p->io.avr, p->r_ctrla) & PRESCALER_gm) >> PRESCALER_gp;
	return 1u << v;	/* DIV1..DIV32768 */
}

/* CPU cycles per RTC counter tick (>=1). */
static uint32_t rtc_cnt_cpu_per_tick(avr_rtc_t *p)
{
	avr_t *avr = p->io.avr;
	uint64_t c = ((uint64_t)avr->frequency * rtc_prescale_div(p)) / rtc_src_hz(p);
	return c ? (uint32_t)c : 1;
}

/* CPU cycles for one PIT period (0 if PIT period is OFF). */
static uint32_t rtc_pit_cpu_cycles(avr_rtc_t *p)
{
	avr_t *avr = p->io.avr;
	uint8_t v = (rd(avr, p->r_pitctrla) & PERIOD_gm) >> PERIOD_gp;
	if (v < 1 || v > 14)
		return 0;	/* OFF */
	uint32_t period = 1u << (v + 1);	/* CYC4..CYC32768 */
	uint64_t c = ((uint64_t)avr->frequency * period) / rtc_src_hz(p);
	return c ? (uint32_t)c : 1;
}

static uint32_t rtc_per(avr_rtc_t *p)
{
	avr_t *avr = p->io.avr;
	return avr->data[p->r_per] | (avr->data[p->r_per + 1] << 8);
}

static uint32_t rtc_cmp(avr_rtc_t *p)
{
	avr_t *avr = p->io.avr;
	return avr->data[p->r_cmp] | (avr->data[p->r_cmp + 1] << 8);
}

static int rtc_cnt_enabled(avr_rtc_t *p)
{
	return (rd(p->io.avr, p->r_ctrla) & RTCEN_bm) != 0;
}

static uint32_t rtc_cnt_now(avr_rtc_t *p)
{
	avr_t *avr = p->io.avr;
	uint32_t top = rtc_per(p);
	if (!p->cnt_cpt)
		return 0;
	uint32_t ticks = (uint32_t)((avr->cycle - p->cnt_start) / p->cnt_cpt);
	return ticks % (top + 1);
}

/* Freeze the live counter value into the CNT registers. While the RTC runs the
 * CNT bytes are only refreshed on explicit CNT reads/writes, so a reschedule
 * triggered by a CTRLA/CLKSEL/PER/CMP write must latch the true count first or
 * it would re-anchor the phase from a stale (often 0) register value. Safe to
 * call when stopped (cnt_cpt==0 → rtc_cnt_now()==0, but we bail anyway). */
static void rtc_latch_cnt(avr_rtc_t *p)
{
	avr_t *avr = p->io.avr;
	if (!p->cnt_cpt || !rtc_cnt_enabled(p))
		return;
	uint16_t cnt = rtc_cnt_now(p);
	avr->data[p->r_cnt] = cnt & 0xff;
	avr->data[p->r_cnt + 1] = cnt >> 8;
}

/* Smallest event count strictly greater than 'from' (compare, or PER+1 wrap). */
static uint32_t rtc_next_target(avr_rtc_t *p, uint32_t from)
{
	uint32_t top = rtc_per(p);
	uint32_t best = top + 1;	/* overflow / wrap */
	uint32_t c = rtc_cmp(p);
	if (c >= 1 && c <= top && c > from && c < best)
		best = c;
	return best;
}

/* Raise the shared RTC_CNT vector for 'flag' if that source is enabled. */
static void rtc_cnt_flag(avr_rtc_t *p, uint8_t flag)
{
	avr_t *avr = p->io.avr;
	set_bits(avr, p->r_intflags, flag);
	uint8_t en = rd(avr, p->r_intctrl);
	if (((flag & OVF_bm) && (en & OVF_bm)) ||
		((flag & CMP_bm) && (en & CMP_bm)))
		avr_raise_interrupt(avr, &p->cnt_vect);
}

static avr_cycle_count_t
avr_rtc_cnt_event(struct avr_t *avr, avr_cycle_count_t when, void *param)
{
	avr_rtc_t *p = (avr_rtc_t *)param;
	uint32_t top = rtc_per(p);
	uint32_t from;

	if (!rtc_cnt_enabled(p))
		return 0;

	if (p->ev_target > top) {
		rtc_cnt_flag(p, OVF_bm);
		p->cnt_start = when;	/* CNT == 0 again */
		from = 0;
		/* CMP == 0 matches at CNT == 0 (the wrap point); the forward-target
		 * search only considers values >= 1, so fire it here. */
		if (rtc_cmp(p) == 0)
			rtc_cnt_flag(p, CMP_bm);
	} else {
		if (rtc_cmp(p) == p->ev_target)
			rtc_cnt_flag(p, CMP_bm);
		from = p->ev_target;
	}

	p->ev_target = rtc_next_target(p, from);
	avr_cycle_count_t next = p->cnt_start +
				(avr_cycle_count_t)p->ev_target * p->cnt_cpt;
	if (next <= when)
		next = when + 1;
	return next;
}

static void
avr_rtc_cnt_reschedule(avr_rtc_t *p)
{
	avr_t *avr = p->io.avr;

	avr_cycle_timer_cancel(avr, avr_rtc_cnt_event, p);
	if (!rtc_cnt_enabled(p))
		return;

	p->cnt_cpt = rtc_cnt_cpu_per_tick(p);

	/* Keep CNT continuity from its current register value. */
	uint32_t cnt = avr->data[p->r_cnt] | (avr->data[p->r_cnt + 1] << 8);
	p->cnt_start = avr->cycle - (avr_cycle_count_t)cnt * p->cnt_cpt;

	p->ev_target = rtc_next_target(p, cnt);
	avr_cycle_count_t abs = p->cnt_start +
				(avr_cycle_count_t)p->ev_target * p->cnt_cpt;
	avr_cycle_count_t rel = (abs > avr->cycle) ? (abs - avr->cycle) : 1;
	avr_cycle_timer_register(avr, rel, avr_rtc_cnt_event, p);
}

static avr_cycle_count_t
avr_rtc_pit_event(struct avr_t *avr, avr_cycle_count_t when, void *param)
{
	avr_rtc_t *p = (avr_rtc_t *)param;

	if (!(rd(avr, p->r_pitctrla) & PITEN_bm) || !p->pit_cpc)
		return 0;	/* stopped */

	/* PI flag + RTC_PIT interrupt (raise gated by PITINTCTRL.PI). */
	avr_raise_interrupt(avr, &p->pit_vect);

	p->pit_start = when;
	p->pit_cpc = rtc_pit_cpu_cycles(p);
	return p->pit_cpc ? (when + p->pit_cpc) : 0;
}

static void
avr_rtc_pit_reschedule(avr_rtc_t *p)
{
	avr_t *avr = p->io.avr;

	avr_cycle_timer_cancel(avr, avr_rtc_pit_event, p);
	p->pit_cpc = rtc_pit_cpu_cycles(p);
	if ((rd(avr, p->r_pitctrla) & PITEN_bm) && p->pit_cpc) {
		p->pit_start = avr->cycle;
		avr_cycle_timer_register(avr, p->pit_cpc, avr_rtc_pit_event, p);
	}
}

static void
avr_rtc_ctrla_write(struct avr_t *avr, avr_io_addr_t addr,
					uint8_t v, void *param)
{
	avr_rtc_t *p = (avr_rtc_t *)param;
	rtc_latch_cnt(p);	/* preserve live count across the reschedule */
	avr_core_watch_write(avr, addr, v);
	rtc_mark_busy(p, BUSY_CTRLA);
	avr_rtc_cnt_reschedule(p);
}

static void
avr_rtc_clksel_write(struct avr_t *avr, avr_io_addr_t addr,
					 uint8_t v, void *param)
{
	avr_rtc_t *p = (avr_rtc_t *)param;
	rtc_latch_cnt(p);	/* preserve live count across the reschedule */
	avr_core_watch_write(avr, addr, v);
	/* Clock source changed: both functions re-derive their period. */
	avr_rtc_cnt_reschedule(p);
	avr_rtc_pit_reschedule(p);
}

static void
avr_rtc_per_write(struct avr_t *avr, avr_io_addr_t addr,
				  uint8_t v, void *param)
{
	avr_rtc_t *p = (avr_rtc_t *)param;
	rtc_latch_cnt(p);	/* preserve live count across the reschedule */
	avr_core_watch_write(avr, addr, v);	/* low or high byte */
	rtc_mark_busy(p, BUSY_PER);
	if (rtc_cnt_enabled(p))
		avr_rtc_cnt_reschedule(p);
}

static void
avr_rtc_cmp_write(struct avr_t *avr, avr_io_addr_t addr,
				  uint8_t v, void *param)
{
	avr_rtc_t *p = (avr_rtc_t *)param;
	rtc_latch_cnt(p);	/* preserve live count across the reschedule */
	avr_core_watch_write(avr, addr, v);	/* low or high byte */
	rtc_mark_busy(p, BUSY_CMP);
	if (rtc_cnt_enabled(p))
		avr_rtc_cnt_reschedule(p);	/* move the compare deadline */
}

static void
avr_rtc_cnt_write(struct avr_t *avr, avr_io_addr_t addr,
				  uint8_t v, void *param)
{
	avr_rtc_t *p = (avr_rtc_t *)param;
	avr_core_watch_write(avr, addr, v);	/* low or high byte */
	rtc_mark_busy(p, BUSY_CNT);
	if (rtc_cnt_enabled(p))
		avr_rtc_cnt_reschedule(p);	/* re-anchor the phase */
}

/* STATUS: synchronisation-busy bits, computed live from the deadlines. */
static uint8_t
avr_rtc_status_read(struct avr_t *avr, avr_io_addr_t addr, void *param)
{
	avr_rtc_t *p = (avr_rtc_t *)param;
	static const uint8_t bm[4] = {
		CTRLABUSY_bm, CNTBUSY_bm, PERBUSY_bm, CMPBUSY_bm
	};
	uint8_t s = 0;
	for (int i = 0; i < 4; i++)
		if (avr->cycle < p->busy_until[i])
			s |= bm[i];
	avr->data[addr] = s;
	return s;
}

static uint8_t
avr_rtc_cnt_read(struct avr_t *avr, avr_io_addr_t addr, void *param)
{
	avr_rtc_t *p = (avr_rtc_t *)param;
	uint16_t cnt = rtc_cnt_enabled(p) ? rtc_cnt_now(p) :
				   (avr->data[p->r_cnt] | (avr->data[p->r_cnt + 1] << 8));
	avr->data[p->r_cnt] = cnt & 0xff;
	avr->data[p->r_cnt + 1] = cnt >> 8;	/* latched high byte */
	return cnt & 0xff;
}

static void
avr_rtc_intctrl_write(struct avr_t *avr, avr_io_addr_t addr,
					  uint8_t v, void *param)
{
	avr_rtc_t *p = (avr_rtc_t *)param;
	avr_core_watch_write(avr, addr, v);
	/* Enabling a source whose flag is already set raises immediately. */
	uint8_t fl = rd(avr, p->r_intflags);
	if (((fl & OVF_bm) && (v & OVF_bm)) || ((fl & CMP_bm) && (v & CMP_bm)))
		avr_raise_interrupt(avr, &p->cnt_vect);
}

static void
avr_rtc_intflags_write(struct avr_t *avr, avr_io_addr_t addr,
					   uint8_t v, void *param)
{
	avr_rtc_t *p = (avr_rtc_t *)param;
	uint8_t res = avr->data[p->r_intflags] & ~v;	/* write-1-to-clear */
	avr_core_watch_write(avr, addr, res);
	if (!(res & (OVF_bm | CMP_bm)))
		avr_clear_interrupt(avr, &p->cnt_vect);
}

static void
avr_rtc_pitctrla_write(struct avr_t *avr, avr_io_addr_t addr,
					   uint8_t v, void *param)
{
	avr_rtc_t *p = (avr_rtc_t *)param;
	avr_core_watch_write(avr, addr, v);
	p->pit_busy_until = avr->cycle + rtc_sync_cpu(p);
	avr_rtc_pit_reschedule(p);
}

/* PITSTATUS: PITCTRLA synchronisation-busy bit, computed from the deadline. */
static uint8_t
avr_rtc_pitstatus_read(struct avr_t *avr, avr_io_addr_t addr, void *param)
{
	avr_rtc_t *p = (avr_rtc_t *)param;
	uint8_t s = (avr->cycle < p->pit_busy_until) ? PIT_CTRLBUSY_bm : 0;
	avr->data[addr] = s;
	return s;
}

static void
avr_rtc_pitintctrl_write(struct avr_t *avr, avr_io_addr_t addr,
						 uint8_t v, void *param)
{
	avr_rtc_t *p = (avr_rtc_t *)param;
	avr_core_watch_write(avr, addr, v);
	if ((v & PI_bm) && (rd(avr, p->r_pitintflags) & PI_bm))
		avr_raise_interrupt(avr, &p->pit_vect);
}

static void
avr_rtc_pitintflags_write(struct avr_t *avr, avr_io_addr_t addr,
						  uint8_t v, void *param)
{
	avr_rtc_t *p = (avr_rtc_t *)param;
	uint8_t res = avr->data[p->r_pitintflags] & ~v;	/* write-1-to-clear */
	avr_core_watch_write(avr, addr, res);
	if (!(res & PI_bm))
		avr_clear_interrupt(avr, &p->pit_vect);
}

static void
avr_rtc_reset(avr_io_t *io)
{
	avr_rtc_t *p = (avr_rtc_t *)io;
	avr_t *avr = p->io.avr;

	avr_cycle_timer_cancel(avr, avr_rtc_cnt_event, p);
	avr_cycle_timer_cancel(avr, avr_rtc_pit_event, p);
	p->cnt_start = 0;
	p->cnt_cpt = 0;
	p->ev_target = 0;
	p->pit_start = 0;
	p->pit_cpc = 0;
	for (int i = 0; i < 4; i++)
		p->busy_until[i] = 0;
	p->pit_busy_until = 0;

	/* PER resets to 0xFFFF (full 16-bit range); everything else to 0. */
	avr->data[p->r_per] = 0xff;
	avr->data[p->r_per + 1] = 0xff;
}

static const char *irq_names[1] = { NULL };

static avr_io_t _io = {
	.kind = "rtc",
	.reset = avr_rtc_reset,
	.irq_names = irq_names,
};

void
avr_rtc_init(
		avr_t * avr,
		avr_rtc_t * p,
		avr_io_addr_t base,
		uint8_t vec_cnt,
		uint8_t vec_pit,
		char name)
{
	memset(p, 0, sizeof(*p));
	p->io = _io;
	p->name = name;
	p->base = base;
	p->r_ctrla = base + RTCR_CTRLA;
	p->r_status = base + RTCR_STATUS;
	p->r_intctrl = base + RTCR_INTCTRL;
	p->r_intflags = base + RTCR_INTFLAGS;
	p->r_clksel = base + RTCR_CLKSEL;
	p->r_cnt = base + RTCR_CNTL;
	p->r_per = base + RTCR_PERL;
	p->r_cmp = base + RTCR_CMPL;
	p->r_pitctrla = base + RTCR_PITCTRLA;
	p->r_pitstatus = base + RTCR_PITSTATUS;
	p->r_pitintctrl = base + RTCR_PITINTCTRL;
	p->r_pitintflags = base + RTCR_PITINTFLAGS;

	/*
	 * RTC_CNT: OVF and CMP both feed the one vector. As with the modern TWI,
	 * .raised is left unset and the flags are set/cleared directly in the
	 * handlers; .enable spans both INTCTRL bits so the engine's enable check
	 * sees "either", and the precise flag/enable pairing is enforced in
	 * rtc_cnt_flag().
	 */
	p->cnt_vect.vector = vec_cnt;
	p->cnt_vect.enable.reg = p->r_intctrl;
	p->cnt_vect.enable.bit = 0;		/* OVF(0) | CMP(1) */
	p->cnt_vect.enable.mask = 0x03;
	p->cnt_vect.raise_sticky = 1;

	/* RTC_PIT: single PI flag → single vector (standard raised/enable bits). */
	p->pit_vect.vector = vec_pit;
	p->pit_vect.enable.reg = p->r_pitintctrl;
	p->pit_vect.enable.bit = 0;		/* PI */
	p->pit_vect.enable.mask = 1;
	p->pit_vect.raised.reg = p->r_pitintflags;
	p->pit_vect.raised.bit = 0;		/* PI */
	p->pit_vect.raised.mask = 1;
	p->pit_vect.raise_sticky = 1;

	avr_register_io(avr, &p->io);
	avr_register_vector(avr, &p->cnt_vect);
	avr_register_vector(avr, &p->pit_vect);

	avr_register_io_write(avr, p->r_ctrla, avr_rtc_ctrla_write, p);
	avr_register_io_write(avr, p->r_clksel, avr_rtc_clksel_write, p);
	avr_register_io_write(avr, p->r_intctrl, avr_rtc_intctrl_write, p);
	avr_register_io_write(avr, p->r_intflags, avr_rtc_intflags_write, p);
	avr_register_io_write(avr, p->r_per, avr_rtc_per_write, p);
	avr_register_io_write(avr, p->r_per + 1, avr_rtc_per_write, p);
	avr_register_io_write(avr, p->r_cmp, avr_rtc_cmp_write, p);
	avr_register_io_write(avr, p->r_cmp + 1, avr_rtc_cmp_write, p);
	avr_register_io_write(avr, p->r_cnt, avr_rtc_cnt_write, p);
	avr_register_io_write(avr, p->r_cnt + 1, avr_rtc_cnt_write, p);
	avr_register_io_read(avr, p->r_cnt, avr_rtc_cnt_read, p);
	avr_register_io_read(avr, p->r_status, avr_rtc_status_read, p);
	avr_register_io_read(avr, p->r_pitstatus, avr_rtc_pitstatus_read, p);
	avr_register_io_write(avr, p->r_pitctrla, avr_rtc_pitctrla_write, p);
	avr_register_io_write(avr, p->r_pitintctrl, avr_rtc_pitintctrl_write, p);
	avr_register_io_write(avr, p->r_pitintflags, avr_rtc_pitintflags_write, p);
}
