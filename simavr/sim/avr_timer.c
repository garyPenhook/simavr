/*
	avr_timer.c

	Handles the 8 bits and 16 bits AVR timer.
	Handles
	+ CDC
	+ Fast PWM

	Copyright 2008-2012 Michel Pollet <buserror@gmail.com>

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
#include <math.h>

#include "avr_timer.h"
#include "avr_ioport.h"
#include "sim_time.h"

/*
 * The timers are /always/ 16 bits here, if the higher byte register
 * is specified it's just added.
 */
static uint16_t
_timer_get_ocr(
		avr_timer_t * p,
		int compi)
{
	return p->io.avr->data[p->comp[compi].r_ocr] |
			  (p->comp[compi].r_ocrh ?
					  (p->io.avr->data[p->comp[compi].r_ocrh] << 8) : 0);
}

static uint16_t
_timer_get_comp_ocr(
		struct avr_t * avr,
		avr_timer_comp_p comp)
{
	int ocrh = comp->r_ocrh;
	return avr->data[comp->r_ocr] |
		(ocrh ? (avr->data[ocrh] << 8) : 0);
}

static inline int
_timer_dual(avr_timer_t * p)
{
	return p->strict && (p->wgm_op_mode_kind == avr_timer_wgm_fc_pwm ||
						 p->wgm_op_mode_kind == avr_timer_wgm_pwm);
}

static inline int
_timer_buffered(avr_timer_t * p)
{
	return _timer_dual(p) || (p->strict && p->wgm_op_mode_kind == avr_timer_wgm_fast_pwm);
}

/* the OCR the compare unit actually uses: the double-buffered copy that was
 * active before a not-yet-latched write, else the register itself */
static uint16_t
_timer_ocr_eff(
		avr_timer_t * p,
		int compi)
{
	if (_timer_buffered(p) && p->comp[compi].ocr_pend)
		return p->comp[compi].ocr_act;
	return _timer_get_ocr(p, compi);
}

/* TOV is set at MAX; in CTC mode the period ends at TOP, which is MAX only
 * when OCRnA is 0xFF (0xFFFF on a 16 bit timer) */
static inline int
_timer_ctc_no_tov(avr_timer_t * p)
{
	return p->strict && p->wgm_op_mode_kind == avr_timer_wgm_ctc &&
			p->tov_top !=(uint16_t)((1UL << p->wgm_op[0].size) - 1);
}

static uint16_t
_timer_get_tcnt(
		avr_timer_t * p)
{
	return p->io.avr->data[p->r_tcnt] |
				(p->r_tcnth ? (p->io.avr->data[p->r_tcnth] << 8) : 0);
}

static uint16_t
_timer_get_icr(
		avr_timer_t * p)
{
	return p->io.avr->data[p->r_icr] |
				(p->r_tcnth ? (p->io.avr->data[p->r_icrh] << 8) : 0);
}
static void
avr_timer_recompute(
		avr_timer_t * p)
{
	uint32_t pre = p->cs_div_value;
	uint16_t top;

	switch (p->wgm_op_mode_kind) {
		case avr_timer_wgm_fast_pwm:
			top = (p->mode.top == avr_timer_wgm_reg_icr) ? _timer_get_icr(p) :
				(p->mode.top == avr_timer_wgm_reg_ocra) ? _timer_ocr_eff(p, AVR_TIMER_COMPA) :
				p->wgm_op_mode_size;
			break;
		case avr_timer_wgm_pwm:
			top = (p->mode.top == avr_timer_wgm_reg_ocra) ?
					_timer_ocr_eff(p, AVR_TIMER_COMPA) : _timer_get_icr(p);
			break;
		default:
			top = p->wgm_op_mode_size;
			break;
	}
	p->tov_top = top;
	p->tov_cycles = _timer_dual(p) ? (uint64_t)pre * 2 * top : (uint64_t)pre * (top + 1);
	for (int i = 0; i < AVR_TIMER_COMP_COUNT; i++) {
		if (!p->comp[i].r_ocr)
			continue;
		uint32_t ocr = _timer_ocr_eff(p, i);
		p->comp[i].comp_cycles = ocr <= top ? (uint64_t)pre * (ocr + 1) : 0;
	}
}

/* Latch the buffered OCRx values (at BOTTOM in fast PWM, at TOP in phase
 * correct PWM) and recompute the next period from them. */
static void
avr_timer_latch_ocr(
		avr_timer_t * p)
{
	int any = 0;
	for (int i = 0; i < AVR_TIMER_COMP_COUNT; i++)
		if (p->comp[i].ocr_pend) {
			p->comp[i].ocr_pend = 0;
			any = 1;
		}
	if (any)
		avr_timer_recompute(p);
}

/* what COMnx does right now: the toggle encoding is reserved in the PWM modes
 * except for OC0A with a variable TOP, and a dual-slope output is inverted on
 * the down-count */
static uint8_t
_timer_com_eff(
		avr_timer_t * p,
		int comp,
		uint8_t mode)
{
	int pwm = p->wgm_op_mode_kind == avr_timer_wgm_pwm ||
			  p->wgm_op_mode_kind == avr_timer_wgm_fast_pwm ||
			  p->wgm_op_mode_kind == avr_timer_wgm_fc_pwm;

	if (pwm && mode == avr_timer_com_toggle &&
			!(comp == AVR_TIMER_COMPA && p->mode.top != avr_timer_wgm_reg_constant))
		return avr_timer_com_normal;
	if (_timer_dual(p) && p->down) {
		if (mode == avr_timer_com_clear)
			return avr_timer_com_set;
		if (mode == avr_timer_com_set)
			return avr_timer_com_clear;
	}
	return mode;
}

static avr_cycle_count_t
avr_timer_comp(
		avr_timer_t *p,
		avr_cycle_count_t when,
		uint8_t comp,
		uint8_t raise_interrupt)
{
	avr_t * avr = p->io.avr;
	uint8_t mode = avr_regbit_get(avr, p->comp[comp].com);
	avr_irq_t * irq = &p->io.irq[TIMER_IRQ_OUT_COMP + comp];
	uint32_t flags = 0;

	if (p->strict)
		mode = _timer_com_eff(p, comp, mode);

	if (raise_interrupt) {
	   avr_raise_interrupt(avr, &p->comp[comp].interrupt);
	}

	// check output compare mode and set/clear pins

	if (p->comp[comp].com_pin.reg)	// we got a physical pin
		flags |= AVR_IOPORT_OUTPUT;
	AVR_LOG(avr, LOG_TRACE, "Timer comp: irq %p, mode %d @%d\n",
			irq, mode, when);
	switch (mode) {
		case avr_timer_com_normal: // Normal mode OCnA disconnected
			break;
		case avr_timer_com_toggle: // Toggle OCnA on compare match
			if (p->comp[comp].com_pin.reg)	// we got a physical pin
				avr_raise_irq(irq,
						flags |
						(avr_regbit_get(avr, p->comp[comp].com_pin) ? 0 : 1));
			else // no pin, toggle the IRQ anyway
				avr_raise_irq(irq,
						p->io.irq[TIMER_IRQ_OUT_COMP + comp].value ? 0 : 1);
			break;
		case avr_timer_com_clear:
			avr_raise_irq(irq, flags | 0);
			break;
		case avr_timer_com_set:
			avr_raise_irq(irq, flags | 1);
			break;
	}

	return p->tov_cycles ? 0 :
				p->comp[comp].comp_cycles ?
						when + p->comp[comp].comp_cycles : 0;
}

static void
avr_timer_comp_on_tov(
		avr_timer_t *p,
		avr_cycle_count_t when,
		uint8_t comp)
{
	avr_t * avr = p->io.avr;

	// check output compare mode and set/clear pins
	uint8_t mode = avr_regbit_get(avr, p->comp[comp].com);
	avr_irq_t * irq = &p->io.irq[TIMER_IRQ_OUT_COMP + comp];
	uint32_t flags = 0;

	if (p->comp[comp].com_pin.reg)	// we got a physical pin
		flags |= AVR_IOPORT_OUTPUT;

	// only PWM modes have special behaviour on overflow
	if((p->wgm_op_mode_kind != avr_timer_wgm_pwm) &&
	   (p->wgm_op_mode_kind != avr_timer_wgm_fast_pwm))
		return;

	switch (mode) {
		case avr_timer_com_normal: // Normal mode
			break;
		case avr_timer_com_toggle: // toggle on compare match => on tov do nothing
			break;
		case avr_timer_com_clear: // clear on compare match => set on tov
			avr_raise_irq(irq, 1 | flags);
			break;
		case avr_timer_com_set: // set on compare match => clear on tov
			avr_raise_irq(irq, 0 | flags);
			break;
	}
}

static avr_cycle_count_t
avr_timer_compa(
		struct avr_t * avr,
		avr_cycle_count_t when,
		void * param)
{
	return avr_timer_comp((avr_timer_t*)param, when, AVR_TIMER_COMPA, 1);
}

static avr_cycle_count_t
avr_timer_compb(
		struct avr_t * avr,
		avr_cycle_count_t when,
		void * param)
{
	return avr_timer_comp((avr_timer_t*)param, when, AVR_TIMER_COMPB, 1);
}

static avr_cycle_count_t
avr_timer_compc(
		struct avr_t * avr,
		avr_cycle_count_t when,
		void * param)
{
	return avr_timer_comp((avr_timer_t*)param, when, AVR_TIMER_COMPC, 1);
}

static const avr_cycle_timer_t timer_comp_dispatch[AVR_TIMER_COMP_COUNT] =
	{ avr_timer_compa, avr_timer_compb, avr_timer_compc };

static void
avr_timer_irq_ext_clock(
		struct avr_irq_t * irq,
		uint32_t value,
		void * param)
{
	avr_timer_t * p = (avr_timer_t *)param;
	avr_t * avr = p->io.avr;

	if ((p->ext_clock_flags & AVR_TIMER_EXTCLK_FLAG_VIRT) || !p->tov_top)
		return;			// we are clocked internally (actually should never come here)

	int bing = 0;
	if (p->ext_clock_flags & AVR_TIMER_EXTCLK_FLAG_EDGE) { // clock on rising edge
		if (!irq->value && value)
			bing++;
	} else {	// clock on falling edge
		if (irq->value && !value)
			bing++;
	}
	if (!bing)
		return;

	//AVR_LOG(avr, LOG_TRACE, "%s Timer%c tick, tcnt=%i\n", __func__, p->name, p->tov_base);

	p->ext_clock_flags |= AVR_TIMER_EXTCLK_FLAG_STARTED;


	int overflow = 0;
	/**
	  *
	  * Datasheet excerpt (Compare Match Output Unit):
	  * "The 16-bit comparator continuously compares TCNT1 with the Output Compare Regis-
		ter (OCR1x). If TCNT equals OCR1x the comparator signals a match. A match will set
		the Output Compare Flag (OCF1x) at the next timer clock cycle. If enabled (OCIE1x =
		1), the Output Compare Flag generates an output compare interrupt."
		Thus, comparators should go before incementing the counter to use counter value
		from the previous cycle.
	*/
	for (int compi = 0; compi < AVR_TIMER_COMP_COUNT; compi++) {
		if (p->wgm_op_mode_kind != avr_timer_wgm_ctc) {
			if ((p->mode.top == avr_timer_wgm_reg_ocra) && (compi == 0))
				continue; // ocra used to define TOP
		}
		if (p->comp[compi].comp_cycles && (p->tov_base == p->comp[compi].comp_cycles)) {
				timer_comp_dispatch[compi](avr, avr->cycle, param);
			if (p->wgm_op_mode_kind == avr_timer_wgm_ctc)
				p->tov_base = 0;
		}
	}

	switch (p->wgm_op_mode_kind) {
		case avr_timer_wgm_fc_pwm:	// in the avr_timer_write_ocr comment "OCR is not used here" - why?
		case avr_timer_wgm_pwm:
			if ((p->ext_clock_flags & AVR_TIMER_EXTCLK_FLAG_REVDIR) != 0) {
				--p->tov_base;
				if (p->tov_base == 0) {
					// overflow occured
					p->ext_clock_flags &= ~AVR_TIMER_EXTCLK_FLAG_REVDIR; // restore forward count direction
					overflow = 1;
				}
			}
			else {
				if (++p->tov_base >= p->tov_top) {
					p->ext_clock_flags |= AVR_TIMER_EXTCLK_FLAG_REVDIR; // prepare to count down
				}
			}
			break;
		case avr_timer_wgm_fast_pwm:
			if (++p->tov_base == p->tov_top) {
				overflow = 1;
				if (p->mode.top == avr_timer_wgm_reg_icr)
					avr_raise_interrupt(avr, &p->icr);
				else if (p->mode.top == avr_timer_wgm_reg_ocra)
					avr_raise_interrupt(avr, &p->comp[0].interrupt);
			}
			else if (p->tov_base > p->tov_top) {
				p->tov_base = 0;
			}
			break;
		case avr_timer_wgm_ctc:
			{
				int max = (1 << p->wgm_op[0].size)-1;
				if (++p->tov_base > max) {
					// overflow occured
					p->tov_base = 0;
					overflow = 1;
				}
			}
			break;
		default:
			if (++p->tov_base > p->tov_top) {
				// overflow occured
				p->tov_base = 0;
				overflow = 1;
			}
			break;
	}

	if (overflow) {
		for (int compi = 0; compi < AVR_TIMER_COMP_COUNT; compi++) {
			if (p->comp[compi].comp_cycles) {
				if (p->mode.top == avr_timer_wgm_reg_ocra && compi == 0)
					continue;
				avr_timer_comp_on_tov(p, 0, compi);
			}
		}
		avr_raise_interrupt(avr, &p->overflow);
	}

}

/* phase correct PWM reached TOP: start counting down, latch the buffered OCRx
 * and arm the down-slope compare matches from the new values */
static avr_cycle_count_t
avr_timer_top(
		struct avr_t * avr,
		avr_cycle_count_t when,
		void * param)
{
	avr_timer_t * p = (avr_timer_t *)param;

	p->down = 1;
	avr_timer_latch_ocr(p);
	for (int compi = 0; compi < AVR_TIMER_COMP_COUNT; compi++) {
		if (!p->comp[compi].r_ocr)
			continue;
		uint32_t ocr = _timer_ocr_eff(p, compi);
		// OCR 0 never matches on the way down (the output stays at its BOTTOM level)
		if (ocr > 0 && ocr <= p->cur_top)
			avr_cycle_timer_register(avr,
					(uint64_t)(p->cur_top - ocr + 1) * p->cs_div_value, timer_comp_dispatch[compi], p);
	}
	return 0;
}

// timer overflow
static avr_cycle_count_t
avr_timer_tov(
		struct avr_t * avr,
		avr_cycle_count_t when,
		void * param)
{
	avr_timer_t * p = (avr_timer_t *)param;
	int start = p->tov_base == 0;

	avr_cycle_count_t next = when;
	if (((p->ext_clock_flags & (AVR_TIMER_EXTCLK_FLAG_AS2 | AVR_TIMER_EXTCLK_FLAG_TN)) != 0)
			&& (p->tov_cycles_fract != 0.0f)) {
		p->phase_accumulator += p->tov_cycles_fract;
		if (p->phase_accumulator >= 1.0f) {
			++next;
			p->phase_accumulator -= 1.0f;
		} else if (p->phase_accumulator <= -1.0f) {
			--next;
			p->phase_accumulator += 1.0f;
		}
	}

	if (!start) {
		// fast PWM latches its buffered OCRx at BOTTOM; a phase correct period starts up-counting
		if (p->strict && _timer_buffered(p) && !_timer_dual(p))
			avr_timer_latch_ocr(p);
		if (_timer_dual(p)) {
			p->cur_top = p->tov_top;
			p->down = 0;
		}
		if (!_timer_ctc_no_tov(p))
			avr_raise_interrupt(avr, &p->overflow);
	}
	p->tov_base = when;


	if (_timer_dual(p)) {
		// dual slope: up-count matches now, the TOP event arms the down-count ones
		uint64_t elapsed = avr->cycle - next;
		uint32_t pre = p->cs_div_value;

		if (start)
			p->down = 0;
		for (int compi = 0; compi < AVR_TIMER_COMP_COUNT; compi++) {
			if (!p->comp[compi].r_ocr || !p->comp[compi].comp_cycles)
				continue;
			if (_timer_ocr_eff(p, compi) < p->cur_top &&
					p->comp[compi].comp_cycles >= elapsed &&
					!(p->block_cmp && p->comp[compi].comp_cycles - elapsed <= pre))
				avr_cycle_timer_register(avr, p->comp[compi].comp_cycles - elapsed,
						timer_comp_dispatch[compi], p);
		}
		if ((uint64_t)p->cur_top * pre >= elapsed)
			avr_cycle_timer_register(avr, (uint64_t)p->cur_top * pre - elapsed, avr_timer_top, p);
		return next + p->tov_cycles;
	}

	for (int compi = 0; compi < AVR_TIMER_COMP_COUNT; compi++) {
		if (p->comp[compi].comp_cycles) {
			if (p->comp[compi].comp_cycles < p->tov_cycles && p->comp[compi].comp_cycles >= (avr->cycle - when) &&
					!(p->strict && p->block_cmp &&
							p->comp[compi].comp_cycles - (avr->cycle - when) <= p->cs_div_value)) {
				avr_timer_comp_on_tov(p, when, compi);
				avr_cycle_timer_register(avr,
					p->comp[compi].comp_cycles - (avr->cycle - next),
					timer_comp_dispatch[compi], p);
			} else if (p->tov_cycles == p->comp[compi].comp_cycles && !start) {
				timer_comp_dispatch[compi](avr, when, param);
				// fast PWM with OCRnx = TOP: the match falls on BOTTOM, whose level
				// wins, so the output stays constant (11.7.3)
				if (p->strict && p->wgm_op_mode_kind == avr_timer_wgm_fast_pwm)
					avr_timer_comp_on_tov(p, when, compi);
			}
		}
	}

	return next + p->tov_cycles;
}

static uint16_t
_avr_timer_get_current_tcnt(
		avr_timer_t * p)
{
	avr_t * avr = p->io.avr;
	if (!(p->ext_clock_flags & (AVR_TIMER_EXTCLK_FLAG_TN | AVR_TIMER_EXTCLK_FLAG_AS2)) ||
			(p->ext_clock_flags & AVR_TIMER_EXTCLK_FLAG_VIRT)
			) {
		if (p->halted)
			return p->halt_tcnt;
		if (p->tov_cycles) {
			uint64_t when = avr->cycle - p->tov_base;

			if (_timer_dual(p) && p->cs_div_value && p->cur_top) {
				uint32_t top = p->cur_top;
				uint64_t pos = (when / p->cs_div_value) % (2 * (uint64_t)top);

				return pos <= top ? pos : 2 * top - pos;
			}
			return (when * (((uint32_t)p->tov_top)+1)) / p->tov_cycles;
		}
	}
	else {
		if (p->tov_top)
			return p->tov_base;
	}
	return 0;
}

static uint8_t
avr_timer_tcnt_read(
		struct avr_t * avr,
		avr_io_addr_t addr,
		void * param)
{
	avr_timer_t * p = (avr_timer_t *)param;
	// made to trigger potential watchpoints

	uint16_t tcnt = _avr_timer_get_current_tcnt(p);

	avr->data[p->r_tcnt] = tcnt;
	if (p->r_tcnth)
		avr->data[p->r_tcnth] = tcnt >> 8;

	return avr_core_watch_read(avr, addr);
}

static inline void
avr_timer_cancel_all_cycle_timers(
		struct avr_t * avr,
		avr_timer_t *timer,
		const uint8_t clear_timers)
{
	if(clear_timers) {
		for (int compi = 0; compi < AVR_TIMER_COMP_COUNT; compi++)
			timer->comp[compi].comp_cycles = 0;
		timer->tov_cycles = 0;
	}


	avr_cycle_timer_cancel(avr, avr_timer_tov, timer);
	avr_cycle_timer_cancel(avr, avr_timer_top, timer);
	avr_cycle_timer_cancel(avr, avr_timer_compa, timer);
	avr_cycle_timer_cancel(avr, avr_timer_compb, timer);
	avr_cycle_timer_cancel(avr, avr_timer_compc, timer);
}

static void
avr_timer_tcnt_write(
		struct avr_t * avr,
		avr_io_addr_t addr,
		uint8_t v,
		void * param)
{
	avr_timer_t * p = (avr_timer_t *)param;
	avr_core_watch_write(avr, addr, v);
	uint16_t tcnt = _timer_get_tcnt(p);

	if (p->halted) {
		// TSM holds the counter: only the held value changes
		p->halt_tcnt = tcnt;
		return;
	}

	if (!p->tov_top)
		return;

	if (tcnt > p->tov_top || (tcnt == p->tov_top && !p->strict))
		tcnt = 0;

	if (!(p->ext_clock_flags & (AVR_TIMER_EXTCLK_FLAG_TN | AVR_TIMER_EXTCLK_FLAG_AS2)) ||
			(p->ext_clock_flags & AVR_TIMER_EXTCLK_FLAG_VIRT)
			) {
		// internal or virtual clock

		// this involves some magicking
		// cancel the current timers, recalculate the "base" we should be at, reset the
		// timer base as it should, and re-schedule the timers using that base.

		avr_timer_cancel_all_cycle_timers(avr, p, 0);

		uint64_t cycles = _timer_dual(p) ?
				(uint64_t)tcnt * p->cs_div_value : (tcnt * p->tov_cycles) / p->tov_top;

		// a TCNT write blocks the compare match on the following timer clock
		p->block_cmp = p->strict && !p->restart;
		if (!p->restart)
			p->down = 0;

		//	printf("%s-%c %d/%d -- cycles %d/%d\n", __FUNCTION__, p->name, tcnt, p->tov_top, (uint32_t)cycles, (uint32_t)p->tov_cycles);

		// this reset the timers bases to the new base
		if (p->tov_cycles > 1) {
			avr_cycle_timer_register(avr, p->tov_cycles - cycles, avr_timer_tov, p);
			p->tov_base = 0;
			avr_timer_tov(avr, avr->cycle - cycles, p);
		}
		p->block_cmp = 0;

		//	tcnt = ((avr->cycle - p->tov_base) * p->tov_top) / p->tov_cycles;
		//	printf("%s-%c new tnt derive to %d\n", __FUNCTION__, p->name, tcnt);
	}
	else {
		// clocked externally
		p->tov_base = tcnt;
	}
}

static void
avr_timer_configure(
		avr_timer_t * p,
		uint32_t prescaler,
		uint32_t top,
		uint8_t reset)
{
	p->tov_top = top;

	avr_t * avr = p->io.avr;
	float resulting_clock = 0.0f; // used only for trace
	float tov_cycles_exact = 0;

	uint8_t as2 = p->ext_clock_flags & AVR_TIMER_EXTCLK_FLAG_AS2;
	uint8_t use_ext_clock = as2 || (p->ext_clock_flags & AVR_TIMER_EXTCLK_FLAG_TN);
	uint8_t virt_ext_clock = use_ext_clock && (p->ext_clock_flags & AVR_TIMER_EXTCLK_FLAG_VIRT);

	if (!use_ext_clock) {
		if (prescaler != 0)
			resulting_clock = (float)avr->frequency / prescaler;
		p->tov_cycles = _timer_dual(p) ? (uint64_t)prescaler * 2 * top : prescaler * (top+1);
		p->cur_top = top;
		p->tov_cycles_fract = 0.0f;
		tov_cycles_exact = p->tov_cycles;
	} else {
		if (!virt_ext_clock) {
			p->tov_cycles = 0;
			p->tov_cycles_fract = 0.0f;
		} else {
			if (prescaler != 0)
				resulting_clock = p->ext_clock / prescaler;
			tov_cycles_exact = (float)avr->frequency / p->ext_clock * prescaler * (top+1);
			// p->tov_cycles = round(tov_cycles_exact); -- don't want libm!
			p->tov_cycles = tov_cycles_exact + .5f; // Round to integer
			p->tov_cycles_fract = tov_cycles_exact - p->tov_cycles;
		}
	}

	if (p->trace) {
		if (!use_ext_clock || virt_ext_clock) {
			// clocked internally
			AVR_LOG(avr, LOG_TRACE, "TIMER: %s-%c TOP %.2fHz = %d cycles = %dusec\n", // TOP there means Timer Overflows Persec ?
					__FUNCTION__, p->name, ((float)avr->frequency / tov_cycles_exact),
					(int)p->tov_cycles, (int)avr_cycles_to_usec(avr, p->tov_cycles));
		} else {
			// clocked externally from the Tn pin
			AVR_LOG(avr, LOG_TRACE, "TIMER: %s-%c use ext clock, TOP=%d\n",
					__FUNCTION__, p->name, (int)p->tov_top
					);
		}
	}

	for (int compi = 0; compi < AVR_TIMER_COMP_COUNT; compi++) {
		if (!p->comp[compi].r_ocr)
			continue;
		uint32_t ocr = _timer_ocr_eff(p, compi);
		//uint32_t comp_cycles = clock * (ocr + 1);
		uint32_t comp_cycles;
		if (virt_ext_clock)
			comp_cycles = (uint32_t)((float)avr->frequency / p->ext_clock * prescaler * (ocr+1));
		else
			comp_cycles = prescaler * (ocr + 1);

		p->comp[compi].comp_cycles = 0;

		if (p->trace & (avr_timer_trace_compa << compi)) {
			if (!use_ext_clock || virt_ext_clock) {
				printf("%s-%c clock %f top %d OCR%c %d\n", __FUNCTION__, p->name,
					resulting_clock, top, 'A'+compi, ocr);
			} else {
				AVR_LOG(avr, LOG_TRACE, "%s timer%c clock via ext pin, TOP=%d OCR%c=%d\n",
						__FUNCTION__, p->name, top, 'A'+compi, ocr);
			}
		}
		if (ocr <= top) {
			p->comp[compi].comp_cycles = comp_cycles;

			if (p->trace & (avr_timer_trace_compa << compi)) printf(
					"TIMER: %s-%c %c %.2fHz = %d cycles\n",
					__FUNCTION__, p->name,
					'A'+compi, resulting_clock / (ocr+1),
					(int)comp_cycles);
		}
	}

	if (!use_ext_clock || virt_ext_clock) {
		if (p->tov_cycles > 1) {
			if (reset) {
				avr_cycle_timer_register(avr, p->tov_cycles, avr_timer_tov, p);
				// calling it once, with when == 0 tells it to arm the A/B/C timers if needed
				p->tov_base = 0;
				avr_timer_tov(avr, avr->cycle, p);
				p->phase_accumulator = 0.0f;
			} else {
				uint64_t orig_tov_base = p->tov_base;
				avr_cycle_timer_register(avr, p->tov_cycles - (avr->cycle - orig_tov_base), avr_timer_tov, p);
				// calling it once, with when == 0 tells it to arm the A/B/C timers if needed
				p->tov_base = 0;
				avr_timer_tov(avr, orig_tov_base, p);
			}
		}
	} else {
		if (reset)
			p->tov_base = 0;
	}

	if (reset) {
		avr_ioport_getirq_t req = {
			.bit = p->ext_clock_pin
		};
		if (avr_ioctl(p->io.avr, AVR_IOCTL_IOPORT_GETIRQ_REGBIT, &req) > 0) {
			// got an IRQ for the Tn input clock pin
			if (use_ext_clock && !virt_ext_clock) {
				if (p->trace)
					AVR_LOG(p->io.avr, LOG_TRACE, "%s: timer%c connecting T%c pin IRQ %d\n", __FUNCTION__, p->name, p->name, req.irq[0]->irq);
				avr_irq_register_notify(req.irq[0], avr_timer_irq_ext_clock, p);
			} else {
				if (p->trace)
					AVR_LOG(p->io.avr, LOG_TRACE, "%s: timer%c disconnecting T%c pin IRQ %d\n", __FUNCTION__, p->name, p->name, req.irq[0]->irq);
				avr_irq_unregister_notify(req.irq[0], avr_timer_irq_ext_clock, p);
			}
		}
	}

}

static void
avr_timer_reconfigure(
		avr_timer_t * p, uint8_t reset)
{
	avr_t * avr = p->io.avr;

	// cancel everything
	avr_timer_cancel_all_cycle_timers(avr, p, 1);

	// TSM holds the prescaler in reset: the new configuration starts when it is released
	if (p->halted)
		return;

	switch (p->wgm_op_mode_kind) {
		case avr_timer_wgm_normal:
			avr_timer_configure(p, p->cs_div_value, p->wgm_op_mode_size, reset);
			break;
		case avr_timer_wgm_fc_pwm:
			avr_timer_configure(p, p->cs_div_value, p->wgm_op_mode_size, reset);
			break;
		case avr_timer_wgm_ctc: {
			avr_timer_configure(p, p->cs_div_value, _timer_get_ocr(p, AVR_TIMER_COMPA), reset);
		}	break;
		case avr_timer_wgm_pwm: {
			uint16_t top = (p->mode.top == avr_timer_wgm_reg_ocra) ?
				_timer_ocr_eff(p, AVR_TIMER_COMPA) : _timer_get_icr(p);
			avr_timer_configure(p, p->cs_div_value, top, reset);
		}	break;
		case avr_timer_wgm_fast_pwm: {
			uint16_t top =
				(p->mode.top == avr_timer_wgm_reg_icr) ? _timer_get_icr(p) :
				(p->mode.top == avr_timer_wgm_reg_ocra) ? _timer_ocr_eff(p, AVR_TIMER_COMPA) :
				p->wgm_op_mode_size;
			avr_timer_configure(p, p->cs_div_value, top, reset);
		}	break;
		case avr_timer_wgm_none:
			avr_timer_configure(p, p->cs_div_value, p->wgm_op_mode_size, reset);
			break;
		default: {
			uint8_t mode = avr_regbit_get_array(avr, p->wgm, ARRAY_SIZE(p->wgm));
			AVR_LOG(avr, LOG_WARNING, "TIMER: %s-%c unsupported timer mode wgm=%d (%d)\n",
					__FUNCTION__, p->name, mode, p->mode.kind);
		}
	}
}

static void
avr_timer_write_ocr(
		struct avr_t * avr,
		avr_io_addr_t addr,
		uint8_t v,
		void * param)
{
	avr_timer_comp_p comp = (avr_timer_comp_p)param;
	avr_timer_t *timer = comp->timer;
	uint16_t oldv;

	/* check to see if the OCR values actually changed */
	oldv = _timer_get_comp_ocr(avr, comp);
	avr_core_watch_write(avr, addr, v);

	if (_timer_buffered(timer) && timer->tov_cycles) {
		// double buffered: the compare unit keeps the old value until it is latched
		if (!comp->ocr_pend)
			comp->ocr_act = oldv;
		comp->ocr_pend = 1;
		return;
	}

	switch (timer->wgm_op_mode_kind) {
		case avr_timer_wgm_normal:
			avr_timer_reconfigure(timer, 0);
			break;
		case avr_timer_wgm_fc_pwm:	// OCR is not used here
			avr_timer_reconfigure(timer, 0);
			break;
		case avr_timer_wgm_ctc:
			avr_timer_reconfigure(timer, 0);
			break;
		case avr_timer_wgm_pwm:
			if (timer->mode.top != avr_timer_wgm_reg_ocra) {
				avr_raise_irq(timer->io.irq + TIMER_IRQ_OUT_PWM0, _timer_get_ocr(timer, AVR_TIMER_COMPA));
			} else {
				avr_timer_reconfigure(timer, 0); // if OCRA is the top, reconfigure needed
			}
			avr_raise_irq(timer->io.irq + TIMER_IRQ_OUT_PWM1, _timer_get_ocr(timer, AVR_TIMER_COMPB));
			if (sizeof(timer->comp)>2)
				avr_raise_irq(timer->io.irq + TIMER_IRQ_OUT_PWM2, _timer_get_ocr(timer, AVR_TIMER_COMPC));
			break;
		case avr_timer_wgm_fast_pwm:
			if (oldv != _timer_get_comp_ocr(avr, comp))
				avr_timer_reconfigure(timer, 0);
			avr_raise_irq(timer->io.irq + TIMER_IRQ_OUT_PWM0,
					_timer_get_ocr(timer, AVR_TIMER_COMPA));
			avr_raise_irq(timer->io.irq + TIMER_IRQ_OUT_PWM1,
					_timer_get_ocr(timer, AVR_TIMER_COMPB));
			if (sizeof(timer->comp)>2)
				avr_raise_irq(timer->io.irq + TIMER_IRQ_OUT_PWM2,
						_timer_get_ocr(timer, AVR_TIMER_COMPC));
			break;
		default:
			AVR_LOG(avr, LOG_WARNING, "TIMER: %s-%c mode %d UNSUPPORTED\n",
					__FUNCTION__, timer->name, timer->mode.kind);
			avr_timer_reconfigure(timer, 0);
			break;
	}
}

static void
avr_timer_write(
		struct avr_t * avr,
		avr_io_addr_t addr,
		uint8_t v,
		void * param)
{
	avr_timer_t * p = (avr_timer_t *)param;

	uint8_t as2 = avr_regbit_get(avr, p->as2);
	uint8_t cs = avr_regbit_get_array(avr, p->cs, ARRAY_SIZE(p->cs));
	uint8_t mode = avr_regbit_get_array(avr, p->wgm, ARRAY_SIZE(p->wgm));

	avr_core_watch_write(avr, addr, v);

	uint8_t new_as2 = avr_regbit_get(avr, p->as2);
	uint8_t new_cs = avr_regbit_get_array(avr, p->cs, ARRAY_SIZE(p->cs));
	uint8_t new_mode = avr_regbit_get_array(avr, p->wgm, ARRAY_SIZE(p->wgm));

	// only reconfigure the timer if "relevant" bits have changed
	// this prevent the timer reset when changing the edge detector
	// or other minor bits
	if (new_cs != cs || new_mode != mode || new_as2 != as2) {
	/* cs */
		if (new_cs == 0) {
			p->cs_div_value = 0;		// reset prescaler
			// cancel everything
			avr_timer_cancel_all_cycle_timers(avr, p, 1);

			AVR_LOG(avr, LOG_TRACE, "TIMER: %s-%c clock turned off\n",
					__func__, p->name);
			return;
		}

		p->ext_clock_flags &= ~(AVR_TIMER_EXTCLK_FLAG_TN | AVR_TIMER_EXTCLK_FLAG_EDGE
								| AVR_TIMER_EXTCLK_FLAG_AS2 | AVR_TIMER_EXTCLK_FLAG_STARTED);
		if (p->ext_clock_pin.reg
				&& (p->cs_div[new_cs] == AVR_TIMER_EXTCLK_CHOOSE)) {
			// Special case: external clock source chosen, prescale divider irrelevant.
			p->cs_div_value = 1;
			p->ext_clock_flags |= AVR_TIMER_EXTCLK_FLAG_TN | (new_cs & AVR_TIMER_EXTCLK_FLAG_EDGE);
		} else {
			p->cs_div_value = 1 << p->cs_div[new_cs];
			if (new_as2) {
				//p->cs_div_value = (uint32_t)((uint64_t)avr->frequency * (1 << p->cs_div[new_cs]) / 32768);
				p->ext_clock_flags |= AVR_TIMER_EXTCLK_FLAG_AS2 | AVR_TIMER_EXTCLK_FLAG_EDGE;
			}
		}

	/* mode */
		p->mode = p->wgm_op[new_mode];
		for (int i = 0; i < AVR_TIMER_COMP_COUNT; i++)
			p->comp[i].ocr_pend = 0;
		p->down = 0;
		p->wgm_op_mode_kind = p->mode.kind;
		p->wgm_op_mode_size = (1 << p->mode.size) - 1;

		avr_timer_reconfigure(p, 1);
	}
}

/*
 * write to the "force output compare" bits
 */
static void avr_timer_write_foc(struct avr_t * avr, avr_io_addr_t addr, uint8_t v, void * param)
{
	avr_timer_t * p = (avr_timer_t *)param;

	/* These are strobe writes: decode them, never store them (FOCnx reads as
	 * zero), and they are only active when the WGM bits select a non-PWM mode */

	for (int compi = 0; compi < AVR_TIMER_COMP_COUNT; compi++) {
		if ((addr == p->comp[compi].foc.reg) &&
				(v & (1 << p->comp[compi].foc.bit))) {
			if (!p->strict) {
				avr_timer_comp(p, avr->cycle, compi, 0);
				continue;
			}
			avr->data[addr] &= ~(1 << p->comp[compi].foc.bit);
			if (p->wgm_op_mode_kind == avr_timer_wgm_normal ||
					p->wgm_op_mode_kind == avr_timer_wgm_ctc)
				avr_timer_comp(p, avr->cycle, compi, 0);
		}
	}
}

/*
 * write to GTCCR: PSR10 restarts the prescaler phase (the counter keeps its
 * value); with TSM set it is held, which halts the counter until TSM is
 * written to zero, when the hardware clears PSR10 and counting resumes.
 */
static void avr_timer_write_gtccr(struct avr_t * avr, avr_io_addr_t addr, uint8_t v, void * param)
{
	avr_timer_t * p = (avr_timer_t *)param;

	avr_core_watch_write(avr, addr, v);

	uint8_t tsm = avr_regbit_get(avr, p->tsm);
	uint8_t psr = avr_regbit_get(avr, p->psr);

	if (tsm && psr) {
		if (!p->halted) {
			p->halt_tcnt = p->cs_div_value ? _avr_timer_get_current_tcnt(p) : _timer_get_tcnt(p);
			avr_timer_cancel_all_cycle_timers(avr, p, 0);
			p->halted = 1;
		}
		return;
	}
	if (p->halted) {
		p->halted = 0;
		if (p->cs_div_value) {
			avr_timer_reconfigure(p, 1);
			avr_timer_tcnt_write(avr, p->r_tcnt, p->halt_tcnt, p);
		}
	} else if (psr && p->cs_div_value && p->tov_cycles && !(_timer_dual(p) && p->down)) {
		// restart the prescaler phase at the current count (on a phase correct down slope the
		// count can not be re-timed this way, so PSR10 is a no-op there)
		p->restart = 1;
		avr_timer_tcnt_write(avr, p->r_tcnt, _avr_timer_get_current_tcnt(p), p);
		p->restart = 0;
	}
	avr_regbit_clear(avr, p->psr);
}

/*
 * write to the TIFR register. Watch for code that writes "1" to clear
 * pending interrupts.
 */
static void
avr_timer_write_pending(
		struct avr_t * avr,
		avr_io_addr_t addr,
		uint8_t v,
		void * param)
{
	avr_timer_t * p = (avr_timer_t *)param;

	// All bits in this register are assumed to be write-1-to-clear.

	if (addr == p->overflow.raised.reg &&
	    avr_regbit_from_value(avr, p->overflow.raised, v)) {
		avr_clear_interrupt(avr, &p->overflow);
	}
	if (addr == p->icr.raised.reg &&
	    avr_regbit_from_value(avr, p->icr.raised, v)) {
		avr_clear_interrupt(avr, &p->icr);
	}

	for (int compi = 0; compi < AVR_TIMER_COMP_COUNT; compi++) {
		if (addr == p->comp[compi].interrupt.raised.reg &&
		    avr_regbit_from_value(avr, p->comp[compi].interrupt.raised,
					  v)) {
			avr_clear_interrupt(avr, &p->comp[compi].interrupt);
		}
	}
}

static void
avr_timer_irq_icp(
		struct avr_irq_t * irq,
		uint32_t value,
		void * param)
{
	avr_timer_t * p = (avr_timer_t *)param;
	avr_t * avr = p->io.avr;

	// input capture disabled when ICR is used as top
	if (p->mode.top == avr_timer_wgm_reg_icr)
		return;
	int bing = 0;
	if (avr_regbit_get(avr, p->ices)) { // rising edge
		if (!irq->value && value)
			bing++;
	} else {	// default, falling edge
		if (irq->value && !value)
			bing++;
	}
	if (!bing)
		return;
	// get current TCNT, copy it to ICR, and raise interrupt
	uint16_t tcnt = _avr_timer_get_current_tcnt(p);
	avr->data[p->r_icr] = tcnt;
	if (p->r_icrh)
		avr->data[p->r_icrh] = tcnt >> 8;
	avr_raise_interrupt(avr, &p->icr);
}

static int
avr_timer_ioctl(
		avr_io_t * port,
		uint32_t ctl,
		void * io_param)
{
	avr_timer_t * p = (avr_timer_t *)port;
	int res = -1;

	if (ctl == AVR_IOCTL_TIMER_SET_TRACE(p->name)) {
		/* Allow setting individual trace flags */
		p->trace = *((uint32_t*)io_param);
		res = 0;
	} else if (ctl == AVR_IOCTL_TIMER_SET_FREQCLK(p->name)) {
		float new_freq = *((float*)io_param);
		if (new_freq >= 0.0f) {
			if (p->as2.reg) {
				if (new_freq <= port->avr->frequency/4) {
					p->ext_clock = new_freq;
					res = 0;
				}
			} else if (p->ext_clock_pin.reg) {
				if (new_freq <= port->avr->frequency/2) {
					p->ext_clock = new_freq;
					res = 0;
				}
			}
		}
	} else if (ctl == AVR_IOCTL_TIMER_SET_VIRTCLK(p->name)) {
		uint8_t new_val = *((uint8_t*)io_param);
		if (!new_val) {
			avr_ioport_getirq_t req_timer_clock_pin = {
				.bit = p->ext_clock_pin
			};
			if (avr_ioctl(p->io.avr, AVR_IOCTL_IOPORT_GETIRQ_REGBIT, &req_timer_clock_pin) > 0) {
				p->ext_clock_flags &= ~AVR_TIMER_EXTCLK_FLAG_VIRT;
				res = 0;
			}
		} else {
			p->ext_clock_flags |= AVR_TIMER_EXTCLK_FLAG_VIRT;
			res = 0;
		}
	}
	if (res >= 0)
		avr_timer_reconfigure(p, 0); // virtual clock: attempt to follow frequency change preserving the phase
	return res;
}

static void
avr_timer_reset(
		avr_io_t * port)
{
	avr_timer_t * p = (avr_timer_t *)port;
	avr_timer_cancel_all_cycle_timers(p->io.avr, p, 0);
	p->halted = p->down = p->block_cmp = 0;
	for (int compi = 0; compi < AVR_TIMER_COMP_COUNT; compi++)
		p->comp[compi].ocr_pend = 0;

	// check to see if the comparators have a pin output. If they do,
	// (try) to get the ioport corresponding IRQ and connect them
	// they will automagically be triggered when the comparator raises
	// it's own IRQ
	for (int compi = 0; compi < AVR_TIMER_COMP_COUNT; compi++) {
		p->comp[compi].comp_cycles = 0;

		avr_ioport_getirq_t req = {
			.bit = p->comp[compi].com_pin
		};
		if (avr_ioctl(port->avr, AVR_IOCTL_IOPORT_GETIRQ_REGBIT, &req) > 0) {
			// cool, got an IRQ
			//printf("%s-%c COMP%c Connecting PIN IRQ %d\n",
			//	__func__, p->name, 'A'+compi, req.irq[0]->irq);
			avr_connect_irq(&port->irq[TIMER_IRQ_OUT_COMP + compi], req.irq[0]);
		}
	}

	avr_irq_register_notify(port->irq + TIMER_IRQ_IN_ICP, avr_timer_irq_icp, p);

	avr_ioport_getirq_t req = {
		.bit = p->icp
	};
	if (avr_ioctl(port->avr, AVR_IOCTL_IOPORT_GETIRQ_REGBIT, &req) > 0) {
		// cool, got an IRQ for the input capture pin
		//printf("%s-%c ICP Connecting PIN IRQ %d\n", __func__, p->name, req.irq[0]->irq);
		avr_connect_irq(req.irq[0], port->irq + TIMER_IRQ_IN_ICP);
	}
	p->ext_clock_flags &= ~(AVR_TIMER_EXTCLK_FLAG_STARTED | AVR_TIMER_EXTCLK_FLAG_TN |
							AVR_TIMER_EXTCLK_FLAG_AS2 | AVR_TIMER_EXTCLK_FLAG_REVDIR);

}

static const char * irq_names[TIMER_IRQ_COUNT] = {
	[TIMER_IRQ_OUT_PWM0] = "8>pwm0",
	[TIMER_IRQ_OUT_PWM1] = "8>pwm1",
	[TIMER_IRQ_OUT_PWM2] = "8>pwm2",
	[TIMER_IRQ_IN_ICP] = "<icp",
	[TIMER_IRQ_OUT_COMP + 0] = ">compa",
	[TIMER_IRQ_OUT_COMP + 1] = ">compb",
	[TIMER_IRQ_OUT_COMP + 2] = ">compc",
};

static	avr_io_t	_io = {
	.kind = "timer",
	.irq_names = irq_names,
	.reset = avr_timer_reset,
	.ioctl = avr_timer_ioctl,
};

void
avr_timer_init(
		avr_t * avr,
		avr_timer_t * p)
{
	p->io = _io;

	avr_register_io(avr, &p->io);
	avr_register_vector(avr, &p->overflow);
	avr_register_vector(avr, &p->icr);

	// allocate this module's IRQ
	avr_io_setirqs(&p->io, AVR_IOCTL_TIMER_GETIRQ(p->name), TIMER_IRQ_COUNT, NULL);

	// marking IRQs as "filtered" means they don't propagate if the
	// new value raised is the same as the last one.. in the case of the
	// pwm value it makes sense not to bother.
	p->io.irq[TIMER_IRQ_OUT_PWM0].flags |= IRQ_FLAG_FILTERED;
	p->io.irq[TIMER_IRQ_OUT_PWM1].flags |= IRQ_FLAG_FILTERED;
	p->io.irq[TIMER_IRQ_OUT_PWM2].flags |= IRQ_FLAG_FILTERED;

	if (p->wgm[0].reg) // these are not present on older AVRs
		avr_register_io_write(avr, p->wgm[0].reg, avr_timer_write, p);
	if (p->wgm[1].reg &&
			(p->wgm[1].reg != p->wgm[0].reg))
		avr_register_io_write(avr, p->wgm[1].reg, avr_timer_write, p);
	if (p->wgm[2].reg &&
			(p->wgm[2].reg != p->wgm[0].reg) &&
			(p->wgm[2].reg != p->wgm[1].reg))
		avr_register_io_write(avr, p->wgm[2].reg, avr_timer_write, p);
	if (p->wgm[3].reg &&
			(p->wgm[3].reg != p->wgm[0].reg) &&
			(p->wgm[3].reg != p->wgm[1].reg) &&
			(p->wgm[3].reg != p->wgm[2].reg))
		avr_register_io_write(avr, p->wgm[3].reg, avr_timer_write, p);

	avr_register_io_write(avr, p->cs[0].reg, avr_timer_write, p);
	if (p->cs[1].reg &&
			(p->cs[1].reg != p->cs[0].reg))
		avr_register_io_write(avr, p->cs[1].reg, avr_timer_write, p);
	if (p->cs[2].reg &&
			(p->cs[2].reg != p->cs[0].reg) && (p->cs[2].reg != p->cs[1].reg))
		avr_register_io_write(avr, p->cs[2].reg, avr_timer_write, p);
	if (p->cs[3].reg &&
			(p->cs[3].reg != p->cs[0].reg) &&
			(p->cs[3].reg != p->cs[1].reg) &&
			(p->cs[3].reg != p->cs[2].reg))
		avr_register_io_write(avr, p->cs[3].reg, avr_timer_write, p);

	if (p->as2.reg) // as2 signifies timer/counter 2... therefore must check for register.
		avr_register_io_write(avr, p->as2.reg, avr_timer_write, p);

	// this assumes all the "pending" interrupt bits are in the same
	// register. Might not be true on all devices ?
	avr_register_io_write(avr, p->overflow.raised.reg, avr_timer_write_pending, p);

	/*
	 * Even if the timer is 16 bits, we don't care to have watches on the
	 * high bytes because the datasheet says that the low address is always
	 * the trigger.
	 */
	for (int compi = 0; compi < AVR_TIMER_COMP_COUNT; compi++) {
		p->comp[compi].timer = p;

		avr_register_vector(avr, &p->comp[compi].interrupt);

		if (p->comp[compi].r_ocr) // not all timers have all comparators
			avr_register_io_write(avr, p->comp[compi].r_ocr, avr_timer_write_ocr, &p->comp[compi]);
		// one handler per FOC register (FOC0A and FOC0B share TCCR0B; a second
		// registration would be muxed in again and strobe twice)
		if (p->comp[compi].foc.reg) {
			int shared = 0;
			for (int c = 0; c < compi; c++)
				shared |= p->comp[c].foc.reg == p->comp[compi].foc.reg;
			if (!shared)
				avr_register_io_write(avr, p->comp[compi].foc.reg, avr_timer_write_foc, p);
		}
	}
	avr_register_io_write(avr, p->r_tcnt, avr_timer_tcnt_write, p);
	avr_register_io_read(avr, p->r_tcnt, avr_timer_tcnt_read, p);
	if (p->psr.reg)
		avr_register_io_write(avr, p->psr.reg, avr_timer_write_gtccr, p);

	if (p->as2.reg) {
		p->ext_clock_flags = AVR_TIMER_EXTCLK_FLAG_VIRT;
		p->ext_clock = 32768.0f;
	} else {
		p->ext_clock_flags = 0;
		p->ext_clock = 0.0f;
	}
}
