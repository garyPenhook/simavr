/*
	avr_vref.c

	"Modern" AVR (AVRxt) Voltage Reference. See avr_vref.h.

	The four CTRL registers store what firmware writes and reset to 0. On a CTRLA
	write the two REFSEL fields are decoded to millivolts and published on the
	ADC0 / DAC0 IRQs so the analog peripherals can pick up the selected reference
	(sim_tiny3217 wires DAC0 to DAC0/AC0). Nothing is published at reset, so a
	device that never programs VREF leaves the peripherals on their own modelled
	reference defaults.

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

#include <string.h>
#include "avr_vref.h"

uint32_t
avr_vref_sel_to_mv(uint8_t sel)
{
	switch (sel & 0x07) {
	case 0x0: return 550;
	case 0x1: return 1100;
	case 0x2: return 2500;
	case 0x3: return 4300;
	case 0x4: return 1500;
	default:  return 0;	/* reserved */
	}
}

static void
avr_vref_ctrla_write(struct avr_t *avr, avr_io_addr_t addr, uint8_t v, void *param)
{
	avr_vref_t *p = (avr_vref_t *)param;

	avr_core_watch_write(avr, addr, v);

	/* Publish the selected reference voltages to the analog peripherals. */
	avr_raise_irq(p->io.irq + AVR_VREF_IRQ_DAC0_MV,
			avr_vref_sel_to_mv((v & VREF_DAC0REFSEL_gm) >> VREF_DAC0REFSEL_gp));
	avr_raise_irq(p->io.irq + AVR_VREF_IRQ_ADC0_MV,
			avr_vref_sel_to_mv((v & VREF_ADC0REFSEL_gm) >> VREF_ADC0REFSEL_gp));
}

static void
avr_vref_store_write(struct avr_t *avr, avr_io_addr_t addr, uint8_t v, void *param)
{
	/* CTRLB force-enable bits and CTRLC/CTRLD (ADC1/DAC1/DAC2 — absent on the
	 * ATtiny3217) have no behavioural effect; store and read back. */
	(void)param;
	avr_core_watch_write(avr, addr, v);
}

static void
avr_vref_reset(avr_io_t *io)
{
	avr_vref_t *p = (avr_vref_t *)io;
	avr_t *avr = p->io.avr;
	for (int i = 0; i < AVR_VREF_REGS; i++)
		avr->data[p->base + i] = 0;	/* all CTRL registers reset to 0x00 */
	/* No IRQ raise at reset: peripherals keep their own reference defaults
	 * until firmware programs VREF.CTRLA. */
}

static const char *irq_names[AVR_VREF_IRQ_COUNT] = {
	[AVR_VREF_IRQ_ADC0_MV] = ">vref.adc0.mv",
	[AVR_VREF_IRQ_DAC0_MV] = ">vref.dac0.mv",
};

static avr_io_t _io = {
	.kind = "vref",
	.reset = avr_vref_reset,
	.irq_names = irq_names,
};

void
avr_vref_init(
		avr_t * avr,
		avr_vref_t * p,
		avr_io_addr_t base,
		char name)
{
	memset(p, 0, sizeof(*p));
	p->io = _io;
	p->name = name;
	p->base = base;

	avr_register_io(avr, &p->io);
	avr_io_setirqs(&p->io, AVR_IOCTL_VREF_GETIRQ(name), AVR_VREF_IRQ_COUNT, NULL);

	avr_register_io_write(avr, base + VREFR_CTRLA, avr_vref_ctrla_write, p);
	avr_register_io_write(avr, base + VREFR_CTRLB, avr_vref_store_write, p);
	avr_register_io_write(avr, base + VREFR_CTRLC, avr_vref_store_write, p);
	avr_register_io_write(avr, base + VREFR_CTRLD, avr_vref_store_write, p);
}
