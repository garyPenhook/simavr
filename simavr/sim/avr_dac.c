/*
	avr_dac.c

	"Modern" AVR (AVRxt) 8-bit Digital-to-Analog Converter. See avr_dac.h.

	The output is recomputed on every CTRLA / DATA write: enabled ?
	(DATA * VREF) / 256 : 0 millivolts. When it changes the new value is raised
	on the OUT IRQ, so it can drive the analog comparator's DAC input, the ADC,
	or be observed.

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
#include "avr_dac.h"

/* CTRLA */
#define ENABLE_bm	0x01
#define OUTEN_bm	0x40

static inline uint8_t rd(avr_t *avr, avr_io_addr_t a) { return avr->data[a]; }

/* Recompute the outputs and publish each if it changed. */
static void dac_update(avr_dac_t *p)
{
	avr_t *avr = p->io.avr;
	uint8_t ctrla = rd(avr, p->r_ctrla);
	uint32_t out = 0;

	if (ctrla & ENABLE_bm)
		out = (uint32_t)rd(avr, p->r_data) * p->vref_mv / 256;

	/* Internal output (to AC/ADC): available whenever ENABLE=1. */
	if (out != p->out_mv) {
		p->out_mv = out;
		avr_raise_irq(p->io.irq + AVR_DAC_IRQ_OUT, out);
	}

	/* Pin output buffer: driven only when ENABLE=1 *and* OUTEN=1; with the
	 * buffer disabled the pin is not driven (DS40002205A 31.3.2.3). */
	uint32_t pin = (ctrla & OUTEN_bm) ? out : 0;
	if (pin != p->pin_mv) {
		p->pin_mv = pin;
		avr_raise_irq(p->io.irq + AVR_DAC_IRQ_PIN, pin);
	}
}

static void
avr_dac_ctrla_write(struct avr_t *avr, avr_io_addr_t addr, uint8_t v, void *param)
{
	avr_dac_t *p = (avr_dac_t *)param;
	avr_core_watch_write(avr, addr, v);
	dac_update(p);
}

static void
avr_dac_data_write(struct avr_t *avr, avr_io_addr_t addr, uint8_t v, void *param)
{
	avr_dac_t *p = (avr_dac_t *)param;
	avr_core_watch_write(avr, addr, v);
	dac_update(p);
}

static void
avr_dac_reset(avr_io_t *io)
{
	avr_dac_t *p = (avr_dac_t *)io;
	p->out_mv = 0;
	p->pin_mv = 0;
}

static const char *irq_names[AVR_DAC_IRQ_COUNT] = {
	[AVR_DAC_IRQ_OUT] = ">dac.out",
	[AVR_DAC_IRQ_PIN] = ">dac.pin",
};

static avr_io_t _io = {
	.kind = "dac",
	.reset = avr_dac_reset,
	.irq_names = irq_names,
};

void
avr_dac_set_vref(avr_dac_t * p, uint32_t vref_mv)
{
	p->vref_mv = vref_mv;
}

void
avr_dac_init(
		avr_t * avr,
		avr_dac_t * p,
		avr_io_addr_t base,
		char name)
{
	memset(p, 0, sizeof(*p));
	p->io = _io;
	p->name = name;
	p->base = base;
	p->r_ctrla = base + DACR_CTRLA;
	p->r_data = base + DACR_DATA;
	p->vref_mv = 1100;

	avr_register_io(avr, &p->io);
	avr_io_setirqs(&p->io, AVR_IOCTL_DAC_GETIRQ(name), AVR_DAC_IRQ_COUNT, NULL);

	avr_register_io_write(avr, p->r_ctrla, avr_dac_ctrla_write, p);
	avr_register_io_write(avr, p->r_data, avr_dac_data_write, p);
}
