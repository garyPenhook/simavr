/*
	avr_clkctrl.c

	"Modern" AVR (AVRxt) Clock Controller (CLKCTRL). See avr_clkctrl.h.

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
#include "avr_clkctrl.h"

/* MCLKCTRLA */
#define CLKSEL_gm	0x03
#define CLKSEL_OSC20M	0x00
#define CLKSEL_OSCULP32K 0x01
#define CLKSEL_XOSC32K	0x02
#define CLKSEL_EXTCLK	0x03

/* MCLKCTRLB */
#define PEN_bm		0x01
#define PDIV_gm		0x1e
#define PDIV_gp		1

/* MCLKLOCK */
#define LOCKEN_bm	0x01

/* MCLKSTATUS */
#define SOSC_bm		0x01
#define OSC20MS_bm	0x10
#define OSC32KS_bm	0x20
#define XOSC32KS_bm	0x40

/* FUSE.OSCCFG */
#define FREQSEL_gm	0x03
#define FREQSEL_16MHZ	0x01

/* PDIV field value (0..0xF) -> division factor. Reserved codes map to 1. */
static const uint8_t pdiv_table[16] = {
	[0x0] = 2,  [0x1] = 4,  [0x2] = 8,  [0x3] = 16,
	[0x4] = 32, [0x5] = 64, [0x8] = 6,  [0x9] = 10,
	[0xa] = 12, [0xb] = 24, [0xc] = 48,
};

static uint32_t
clkctrl_base_osc20m(avr_clkctrl_t *p)
{
	if (p->freq_osc20m)
		return p->freq_osc20m;
	return 20000000;	/* derived value filled in at init; safety net */
}

static void
avr_clkctrl_recompute(avr_clkctrl_t *p)
{
	avr_t *avr = p->io.avr;
	uint8_t ctrla = avr->data[p->r_mclkctrla];
	uint8_t ctrlb = avr->data[p->r_mclkctrlb];
	uint32_t base;
	uint8_t status = 0;

	switch (ctrla & CLKSEL_gm) {
	case CLKSEL_OSC20M:
		base = clkctrl_base_osc20m(p);
		status = OSC20MS_bm;
		break;
	case CLKSEL_OSCULP32K:
		base = p->freq_osc32k;
		status = OSC32KS_bm;
		break;
	case CLKSEL_XOSC32K:
		base = p->freq_osc32k;
		status = XOSC32KS_bm;
		break;
	case CLKSEL_EXTCLK:
	default:
		base = p->freq_extclk ? p->freq_extclk : avr->frequency;
		break;
	}

	uint32_t div = 1;
	if (ctrlb & PEN_bm)
		div = pdiv_table[(ctrlb & PDIV_gm) >> PDIV_gp];
	if (!div)
		div = 1;

	if (base)
		avr->frequency = base / div;

	/* The selected source is "stable" in this model. */
	avr_core_watch_write(avr, p->r_mclkstatus, status);
}

/* A protected write is honoured only inside the CCP window and while unlocked. */
static int
clkctrl_locked(avr_clkctrl_t *p)
{
	return (p->io.avr->data[p->r_mclklock] & LOCKEN_bm) != 0;
}

static void
avr_clkctrl_mclkctrla_write(struct avr_t *avr, avr_io_addr_t addr,
							uint8_t v, void *param)
{
	avr_clkctrl_t *p = (avr_clkctrl_t *)param;
	if (clkctrl_locked(p) || !avr_ccp_io_write_enabled(avr))
		return;	/* ignored by hardware */
	avr_core_watch_write(avr, p->r_mclkctrla, v);
	avr_clkctrl_recompute(p);
}

static void
avr_clkctrl_mclkctrlb_write(struct avr_t *avr, avr_io_addr_t addr,
							uint8_t v, void *param)
{
	avr_clkctrl_t *p = (avr_clkctrl_t *)param;
	if (clkctrl_locked(p) || !avr_ccp_io_write_enabled(avr))
		return;
	avr_core_watch_write(avr, p->r_mclkctrlb, v & (PEN_bm | PDIV_gm));
	avr_clkctrl_recompute(p);
}

static void
avr_clkctrl_mclklock_write(struct avr_t *avr, avr_io_addr_t addr,
						   uint8_t v, void *param)
{
	avr_clkctrl_t *p = (avr_clkctrl_t *)param;
	/* LOCKEN is itself CCP-protected and, once set, sticks until reset. */
	if (!avr_ccp_io_write_enabled(avr))
		return;
	if (clkctrl_locked(p))
		return;
	avr_core_watch_write(avr, p->r_mclklock, v & LOCKEN_bm);
}

/* MCLKSTATUS is read-only; swallow writes so firmware can't corrupt it. */
static void
avr_clkctrl_mclkstatus_write(struct avr_t *avr, avr_io_addr_t addr,
							 uint8_t v, void *param)
{
	(void)avr; (void)addr; (void)v; (void)param;
}

static void
avr_clkctrl_reset(avr_io_t *io)
{
	avr_clkctrl_t *p = (avr_clkctrl_t *)io;
	avr_t *avr = p->io.avr;

	/*
	 * Reset defaults: OSC20M selected, prescaler enabled dividing by 6
	 * (MCLKCTRLB = PEN | PDIV_6X), giving the documented ~3.33 MHz CLK_PER on
	 * a 20 MHz part. recompute() then sets avr->frequency accordingly.
	 */
	avr->data[p->r_mclkctrla] = CLKSEL_OSC20M;
	avr->data[p->r_mclkctrlb] = PEN_bm | (0x08 << PDIV_gp);	/* PDIV = 6X */
	avr->data[p->r_mclklock] = 0;
	avr_clkctrl_recompute(p);
}

static const char *irq_names[1] = { NULL };

static avr_io_t _io = {
	.kind = "clkctrl",
	.reset = avr_clkctrl_reset,
	.irq_names = irq_names,
};

void
avr_clkctrl_init(
		avr_t * avr,
		avr_clkctrl_t * p,
		avr_io_addr_t base,
		uint8_t osccfg_fuse_index)
{
	memset(p, 0, sizeof(*p));
	p->io = _io;
	p->base = base;
	p->r_mclkctrla = base + CLKCTRL_MCLKCTRLA_O;
	p->r_mclkctrlb = base + CLKCTRL_MCLKCTRLB_O;
	p->r_mclklock = base + CLKCTRL_MCLKLOCK_O;
	p->r_mclkstatus = base + CLKCTRL_MCLKSTATUS_O;

	/* Resolve the OSC20M base frequency from the OSCCFG fuse (FREQSEL). */
	uint8_t osccfg = (osccfg_fuse_index == 0xff) ? 0 :
					 avr->fuse[osccfg_fuse_index];
	p->freq_osc20m = ((osccfg & FREQSEL_gm) == FREQSEL_16MHZ) ?
					 16000000 : 20000000;
	p->freq_osc32k = 32768;
	p->freq_extclk = 0;

	avr_register_io(avr, &p->io);

	avr_register_io_write(avr, p->r_mclkctrla, avr_clkctrl_mclkctrla_write, p);
	avr_register_io_write(avr, p->r_mclkctrlb, avr_clkctrl_mclkctrlb_write, p);
	avr_register_io_write(avr, p->r_mclklock, avr_clkctrl_mclklock_write, p);
	avr_register_io_write(avr, p->r_mclkstatus, avr_clkctrl_mclkstatus_write, p);
}
