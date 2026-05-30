/*
	avr_adc_modern.c

	"Modern" AVR (AVRxt) register-block ADC. See avr_adc_modern.h.

	A conversion is started by writing COMMAND.STCONV (with CTRLA.ENABLE). The
	MUXPOS-selected channel's analog input (millivolts, supplied via the AINn
	IRQs) is converted against vref into an 8- or 10-bit result after a realistic
	delay (~13 ADC clocks at the CTRLC prescaler), modelled with a simavr cycle
	timer. On completion RES is updated, RESRDY is flagged (interrupt raised if
	enabled), the window comparator (CTRLE.WINCM) is evaluated, and — in
	free-running mode (CTRLA.FREERUN) — another conversion is queued.

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
#include "avr_adc_modern.h"
#include "sim_cycle_timers.h"

/* CTRLA */
#define ENABLE_bm	0x01
#define FREERUN_bm	0x02
#define RESSEL_bm	0x04	/* 1 = 8-bit, 0 = 10-bit */

/* CTRLB */
#define SAMPNUM_gm	0x07	/* accumulate 1<<SAMPNUM samples */

/* CTRLC */
#define PRESC_gm	0x07
#define REFSEL_gm	0x30
#define REFSEL_gp	4
#define REFSEL_INTREF	0	/* internal reference (VREF); 1=VDD, 2=VREFA */

/* CTRLE */
#define WINCM_gm	0x07
#define WINCM_NONE	0
#define WINCM_BELOW	1
#define WINCM_ABOVE	2
#define WINCM_INSIDE	3
#define WINCM_OUTSIDE	4

/* COMMAND */
#define STCONV_bm	0x01

/* INTCTRL / INTFLAGS */
#define RESRDY_bm	0x01
#define WCMP_bm		0x02

static inline uint8_t rd(avr_t *avr, avr_io_addr_t a) { return avr->data[a]; }
static void set_bits(avr_t *avr, avr_io_addr_t a, uint8_t m)
{
	avr_core_watch_write(avr, a, avr->data[a] | m);
}
static uint16_t rd16(avr_t *avr, avr_io_addr_t a)
{
	return avr->data[a] | (avr->data[a + 1] << 8);
}

static int adc_enabled(avr_adc_modern_t *p)
{
	return (rd(p->io.avr, p->r_ctrla) & ENABLE_bm) != 0;
}

/* Number of samples accumulated per conversion: 1<<SAMPNUM (1..64). */
static uint16_t adc_sampnum(avr_adc_modern_t *p)
{
	return 1u << (rd(p->io.avr, p->r_ctrlb) & SAMPNUM_gm);
}

/* The input voltage (mV) for the MUXPOS-selected channel. GND reads 0; the
 * other internal sources (DAC0 / INTREF / temp) use their settable input. */
static uint32_t adc_channel_mv(avr_adc_modern_t *p)
{
	uint8_t ch = rd(p->io.avr, p->r_muxpos) & 0x1f;
	if (ch == AVR_ADCM_CH_GND)
		return 0;
	return ch < AVR_ADCM_CHANNELS ? p->chan_mv[ch] : 0;
}

/* The active reference voltage (mV) per CTRLC.REFSEL. */
static uint32_t adc_ref_mv(avr_adc_modern_t *p)
{
	uint8_t sel = (rd(p->io.avr, p->r_ctrlc) & REFSEL_gm) >> REFSEL_gp;
	uint32_t ref = (sel == REFSEL_INTREF) ? p->intref_mv : p->vref_mv;
	return ref ? ref : 3300;
}

/* One sample of the selected channel, as an 8- or 10-bit code. */
static uint32_t adc_one_sample(avr_adc_modern_t *p)
{
	avr_t *avr = p->io.avr;
	uint32_t vref = adc_ref_mv(p);
	uint32_t maxc = (rd(avr, p->r_ctrla) & RESSEL_bm) ? 255 : 1023;
	uint32_t res = (adc_channel_mv(p) * (maxc + 1)) / vref;
	return res > maxc ? maxc : res;
}

/* Conversion duration in CPU cycles: ~13 ADC clocks at the CTRLC prescaler. */
static uint32_t adc_conv_cycles(avr_adc_modern_t *p)
{
	uint8_t presc = rd(p->io.avr, p->r_ctrlc) & PRESC_gm;
	uint32_t div = 2u << presc;	/* DIV2..DIV256 */
	return 13u * div;
}

/* Evaluate the window comparator against the freshly-stored result. */
static void adc_window_compare(avr_adc_modern_t *p, uint16_t res)
{
	avr_t *avr = p->io.avr;
	uint8_t mode = rd(avr, p->r_ctrle) & WINCM_gm;
	uint16_t lt = rd16(avr, p->r_winlt);
	uint16_t ht = rd16(avr, p->r_winht);
	int hit = 0;

	switch (mode) {
	case WINCM_BELOW:	hit = res < lt; break;
	case WINCM_ABOVE:	hit = res > ht; break;
	case WINCM_INSIDE:	hit = res >= lt && res <= ht; break;
	case WINCM_OUTSIDE:	hit = res < lt || res > ht; break;
	case WINCM_NONE:
	default:		return;
	}
	if (hit) {
		set_bits(avr, p->r_intflags, WCMP_bm);
		if (rd(avr, p->r_intctrl) & WCMP_bm)
			avr_raise_interrupt(avr, &p->wcomp);
	}
}

/* Finish an accumulation burst: store the summed result, flag RESRDY, run the
 * window comparator and clear STCONV. */
static void adc_finish(avr_adc_modern_t *p)
{
	avr_t *avr = p->io.avr;
	uint32_t res = p->acc_sum > 0xffff ? 0xffff : p->acc_sum;

	avr_core_watch_write(avr, p->r_res, res & 0xff);
	avr_core_watch_write(avr, p->r_res + 1, (res >> 8) & 0xff);

	/* STCONV self-clears when the conversion completes. */
	avr_core_watch_write(avr, p->r_command,
						 rd(avr, p->r_command) & ~STCONV_bm);

	set_bits(avr, p->r_intflags, RESRDY_bm);
	if (rd(avr, p->r_intctrl) & RESRDY_bm)
		avr_raise_interrupt(avr, &p->resrdy);

	adc_window_compare(p, (uint16_t)res);
}

static avr_cycle_count_t
avr_adc_modern_complete(struct avr_t *avr, avr_cycle_count_t when, void *param)
{
	avr_adc_modern_t *p = (avr_adc_modern_t *)param;

	if (!adc_enabled(p))
		return 0;

	/* Accumulate one sample of the burst. */
	p->acc_sum += adc_one_sample(p);
	p->acc_count++;
	if (p->acc_count < p->acc_target)
		return when + adc_conv_cycles(p);	/* more samples to take */

	adc_finish(p);

	/* Free-running mode immediately starts the next accumulation burst. */
	if (rd(avr, p->r_ctrla) & FREERUN_bm) {
		p->acc_sum = 0;
		p->acc_count = 0;
		p->acc_target = adc_sampnum(p);
		return when + adc_conv_cycles(p);
	}
	return 0;
}

static void
avr_adc_modern_start(avr_adc_modern_t *p)
{
	avr_t *avr = p->io.avr;
	avr_cycle_timer_cancel(avr, avr_adc_modern_complete, p);
	if (!adc_enabled(p))
		return;
	p->acc_sum = 0;
	p->acc_count = 0;
	p->acc_target = adc_sampnum(p);
	avr_cycle_timer_register(avr, adc_conv_cycles(p),
							 avr_adc_modern_complete, p);
}

static void
avr_adc_modern_command_write(struct avr_t *avr, avr_io_addr_t addr,
							 uint8_t v, void *param)
{
	avr_adc_modern_t *p = (avr_adc_modern_t *)param;
	avr_core_watch_write(avr, addr, v);
	if ((v & STCONV_bm) && adc_enabled(p))
		avr_adc_modern_start(p);
}

static void
avr_adc_modern_ctrla_write(struct avr_t *avr, avr_io_addr_t addr,
						   uint8_t v, void *param)
{
	avr_adc_modern_t *p = (avr_adc_modern_t *)param;
	uint8_t was_free = avr->data[addr] & FREERUN_bm;
	avr_core_watch_write(avr, addr, v);

	if (!(v & ENABLE_bm)) {
		avr_cycle_timer_cancel(avr, avr_adc_modern_complete, p);
		return;
	}
	/* Entering free-running mode starts the conversion stream. */
	if ((v & FREERUN_bm) && !was_free)
		avr_adc_modern_start(p);
}

static void
avr_adc_modern_intflags_write(struct avr_t *avr, avr_io_addr_t addr,
							  uint8_t v, void *param)
{
	avr_adc_modern_t *p = (avr_adc_modern_t *)param;
	uint8_t res = avr->data[addr] & ~v;	/* write-1-to-clear */
	avr_core_watch_write(avr, addr, res);
	if (!(res & RESRDY_bm))
		avr_clear_interrupt(avr, &p->resrdy);
	if (!(res & WCMP_bm))
		avr_clear_interrupt(avr, &p->wcomp);
}

/* A board/test presents the analog voltage (mV) on a channel. */
static void
avr_adc_modern_irq_input(struct avr_irq_t *irq, uint32_t value, void *param)
{
	avr_adc_modern_t *p = (avr_adc_modern_t *)param;
	int ch = irq->irq - p->base_irq;
	if (ch >= 0 && ch < AVR_ADCM_CHANNELS)
		p->chan_mv[ch] = value;
}

static void
avr_adc_modern_reset(avr_io_t *io)
{
	avr_adc_modern_t *p = (avr_adc_modern_t *)io;
	avr_cycle_timer_cancel(p->io.avr, avr_adc_modern_complete, p);
}

static const char *irq_names[AVR_ADCM_CHANNELS] = {
	"adc.ain0",  "adc.ain1",  "adc.ain2",  "adc.ain3",
	"adc.ain4",  "adc.ain5",  "adc.ain6",  "adc.ain7",
	"adc.ain8",  "adc.ain9",  "adc.ain10", "adc.ain11",
	"adc.ain12", "adc.ain13", "adc.ain14", "adc.ain15",
	"adc.ain16", "adc.ain17", "adc.ain18", "adc.ain19",
	"adc.ain20", "adc.ain21", "adc.ain22", "adc.ain23",
	"adc.ain24", "adc.ain25", "adc.ain26", "adc.ain27",
	[AVR_ADCM_CH_DAC0]	= "adc.dac0",
	[AVR_ADCM_CH_INTREF]	= "adc.intref",
	[AVR_ADCM_CH_TEMPSENSE]	= "adc.temp",
	[AVR_ADCM_CH_GND]	= "adc.gnd",
};

static avr_io_t _io = {
	.kind = "adc_modern",
	.reset = avr_adc_modern_reset,
	.irq_names = irq_names,
};

void
avr_adc_modern_set_vref(avr_adc_modern_t * p, uint32_t vref_mv)
{
	p->vref_mv = vref_mv;
}

void
avr_adc_modern_set_intref(avr_adc_modern_t * p, uint32_t intref_mv)
{
	p->intref_mv = intref_mv;
}

void
avr_adc_modern_init(
		avr_t * avr,
		avr_adc_modern_t * p,
		avr_io_addr_t base,
		uint8_t vec_resrdy,
		uint8_t vec_wcomp,
		char name)
{
	memset(p, 0, sizeof(*p));
	p->io = _io;
	p->name = name;
	p->base = base;
	p->r_ctrla = base + ADCMR_CTRLA;
	p->r_ctrlb = base + ADCMR_CTRLB;
	p->r_ctrlc = base + ADCMR_CTRLC;
	p->r_ctrle = base + ADCMR_CTRLE;
	p->r_muxpos = base + ADCMR_MUXPOS;
	p->r_command = base + ADCMR_COMMAND;
	p->r_intctrl = base + ADCMR_INTCTRL;
	p->r_intflags = base + ADCMR_INTFLAGS;
	p->r_res = base + ADCMR_RESL;
	p->r_winlt = base + ADCMR_WINLTL;
	p->r_winht = base + ADCMR_WINHTL;
	p->vref_mv = 3300;
	p->intref_mv = 3300;	/* until VREF.ADC0REFSEL is programmed */

	/* RESRDY: enabled by INTCTRL.RESRDY(0), flagged in INTFLAGS.RESRDY(0). */
	p->resrdy.vector = vec_resrdy;
	p->resrdy.enable.reg = p->r_intctrl;
	p->resrdy.enable.bit = 0;
	p->resrdy.enable.mask = 1;
	p->resrdy.raised.reg = p->r_intflags;
	p->resrdy.raised.bit = 0;
	p->resrdy.raised.mask = 1;
	p->resrdy.raise_sticky = 1;

	/* WCOMP: enabled by INTCTRL.WCMP(1), flagged in INTFLAGS.WCMP(1). */
	p->wcomp.vector = vec_wcomp;
	p->wcomp.enable.reg = p->r_intctrl;
	p->wcomp.enable.bit = 1;
	p->wcomp.enable.mask = 1;
	p->wcomp.raised.reg = p->r_intflags;
	p->wcomp.raised.bit = 1;
	p->wcomp.raised.mask = 1;
	p->wcomp.raise_sticky = 1;

	avr_register_io(avr, &p->io);
	avr_register_vector(avr, &p->resrdy);
	avr_register_vector(avr, &p->wcomp);

	avr_io_setirqs(&p->io, AVR_IOCTL_ADCM_GETIRQ(name),
				   AVR_ADCM_CHANNELS, NULL);
	p->base_irq = p->io.irq[0].irq;
	for (int i = 0; i < AVR_ADCM_CHANNELS; i++)
		avr_irq_register_notify(p->io.irq + i, avr_adc_modern_irq_input, p);

	avr_register_io_write(avr, p->r_ctrla, avr_adc_modern_ctrla_write, p);
	avr_register_io_write(avr, p->r_command, avr_adc_modern_command_write, p);
	avr_register_io_write(avr, p->r_intflags, avr_adc_modern_intflags_write, p);
}
