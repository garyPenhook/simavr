/*
	avr_clkctrl_dd.c

	AVR DD Clock Controller (CLKCTRL). See avr_clkctrl_dd.h.

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
#include "avr_clkctrl_dd.h"

/* MCLKCTRLA */
#define CLKSEL_gm		0x0f
#define CLKSEL_OSCHF	0x0
#define CLKSEL_OSC32K	0x1
#define CLKSEL_XOSC32K	0x2
#define CLKSEL_EXTCLK	0x3
#define CLKOUT_bm		0x80

/* MCLKCTRLB */
#define PEN_bm			0x01
#define PDIV_gm			0x1e
#define PDIV_gp			1

/* MCLKCTRLC */
#define CFDEN_bm		0x01
#define CFDTST_bm		0x02
#define CFDSRC_gm		0x0c

/* MCLKINTCTRL / MCLKINTFLAGS */
#define INTTYPE_bm		0x80
#define CFD_bm			0x01

/* MCLKSTATUS */
#define SOSC_bm			0x01
#define OSCHFS_bm		0x02
#define OSC32KS_bm		0x04
#define XOSC32KS_bm		0x08
#define EXTS_bm			0x10
#define PLLS_bm			0x20

/* OSCHFCTRLA */
#define OSCHF_FRQSEL_gm	0x3c
#define OSCHF_FRQSEL_gp	2
#define OSCHF_RESET		0x0c	/* FRQSEL = 4 MHz (DS40002413B 12.5.7) */

/* PLLCTRLA */
#define PLL_MULFAC_gm	0x03

/* XOSC32KCTRLA / XOSCHFCTRLA */
#define XOSC_ENABLE_bm	0x01

/* FUSE.OSCCFG */
#define OSCCFG_CLKSEL_gm 0x0f

/* PDIV field value (0..0xF) -> division factor; reserved codes read as 0. */
static const uint8_t pdiv_table[16] = {
	[0x0] = 2,  [0x1] = 4,  [0x2] = 8,  [0x3] = 16,
	[0x4] = 32, [0x5] = 64, [0x8] = 6,  [0x9] = 10,
	[0xa] = 12, [0xb] = 24, [0xc] = 48,
};

/* OSCHFCTRLA.FRQSEL -> MHz; 0 for the reserved codes (DS40002413B 12.5.7). */
static const uint8_t frqsel_mhz[16] = {
	[0x0] = 1, [0x1] = 2, [0x2] = 3, [0x3] = 4,
	[0x5] = 8, [0x6] = 12, [0x7] = 16, [0x8] = 20, [0x9] = 24,
};

static inline uint8_t
reg(avr_clkctrl_dd_t * p, int off)
{
	return p->io.avr->data[p->base + off];
}

static inline void
set(avr_clkctrl_dd_t * p, int off, uint8_t v)
{
	avr_core_watch_write(p->io.avr, p->base + off, v);
}

/* Frequency of the source selected by MCLKCTRLA.CLKSEL, or 0 when that source
 * is not (yet) running. */
static uint32_t
clkctrl_dd_source(avr_clkctrl_dd_t * p)
{
	switch (reg(p, CLKCTRL_DD_MCLKCTRLA_O) & CLKSEL_gm) {
	case CLKSEL_OSCHF: {
		uint32_t mhz = frqsel_mhz[(reg(p, CLKCTRL_DD_OSCHFCTRLA_O) &
									OSCHF_FRQSEL_gm) >> OSCHF_FRQSEL_gp];
		return mhz * 1000000;
	}
	case CLKSEL_OSC32K:
		return 32768;
	case CLKSEL_XOSC32K:
		/* Only switches once the crystal/clock is enabled and running. */
		return (reg(p, CLKCTRL_DD_XOSC32KCTRLA_O) & XOSC_ENABLE_bm) ?
				(p->freq_xosc32k ? p->freq_xosc32k : 32768) : 0;
	case CLKSEL_EXTCLK:
		/* The external frequency is not known to the model unless the embedder
		 * sets freq_extclk: then CLK_MAIN keeps its current rate (as avr_clkctrl.c). */
		return (reg(p, CLKCTRL_DD_XOSCHFCTRLA_O) & XOSC_ENABLE_bm) ?
				(p->freq_extclk ? p->freq_extclk : p->io.avr->frequency) : 0;
	}
	return 0;	/* reserved CLKSEL */
}

static void
clkctrl_dd_recompute(avr_clkctrl_dd_t * p)
{
	avr_t * avr = p->io.avr;
	uint8_t ctrlb = reg(p, CLKCTRL_DD_MCLKCTRLB_O);
	uint8_t status = 0;

	/* OSCHF and OSC32K run whenever requested, so they are always "stable" here
	 * (start-up times are not modelled). */
	status |= OSCHFS_bm | OSC32KS_bm;
	if (reg(p, CLKCTRL_DD_XOSC32KCTRLA_O) & XOSC_ENABLE_bm)
		status |= XOSC32KS_bm;
	if (reg(p, CLKCTRL_DD_XOSCHFCTRLA_O) & XOSC_ENABLE_bm)
		status |= EXTS_bm;
	if (reg(p, CLKCTRL_DD_PLLCTRLA_O) & PLL_MULFAC_gm)
		status |= PLLS_bm;

	uint32_t base = clkctrl_dd_source(p);
	/* A switch to a source that is not running yet is pending: SOSC stays set and
	 * CLK_MAIN keeps its current source (DS40002413B 12.3.2/12.5.6). */
	if (!base)
		status |= SOSC_bm;

	if (base) {
		uint32_t div = 1;
		if (ctrlb & PEN_bm)
			div = pdiv_table[(ctrlb & PDIV_gm) >> PDIV_gp];
		if (!div)
			div = 1;
		avr->frequency = base / div;
	}
	set(p, CLKCTRL_DD_MCLKSTATUS_O, status);
}

/* Every register except MCLKSTATUS/MCLKINTFLAGS/OSCHFTUNE is CCP protected. */
static void
clkctrl_dd_write(struct avr_t * avr, avr_io_addr_t addr, uint8_t v, void * param)
{
	avr_clkctrl_dd_t * p = (avr_clkctrl_dd_t *)param;
	int off = addr - p->base;
	uint8_t mask;

	switch (off) {
	case CLKCTRL_DD_MCLKCTRLA_O:	mask = CLKOUT_bm | CLKSEL_gm; break;
	case CLKCTRL_DD_MCLKCTRLB_O:	mask = PDIV_gm | PEN_bm; break;
	case CLKCTRL_DD_MCLKCTRLC_O:	mask = CFDSRC_gm | CFDTST_bm | CFDEN_bm; break;
	case CLKCTRL_DD_MCLKINTCTRL_O:	mask = INTTYPE_bm | CFD_bm; break;
	case CLKCTRL_DD_OSCHFCTRLA_O:	mask = 0x80 | OSCHF_FRQSEL_gm | 0x01; break;
	case CLKCTRL_DD_PLLCTRLA_O:		mask = 0x80 | 0x40 | PLL_MULFAC_gm; break;
	case CLKCTRL_DD_OSC32KCTRLA_O:	mask = 0x80; break;
	case CLKCTRL_DD_XOSC32KCTRLA_O:	mask = 0x80 | 0x30 | 0x04 | 0x02 | 0x01; break;
	case CLKCTRL_DD_XOSCHFCTRLA_O:	mask = 0x80 | 0x30 | 0x0c | 0x02 | 0x01; break;
	default:
		return;
	}
	if (!avr_ccp_io_write_enabled(avr))
		return;	/* ignored by hardware */
	set(p, off, v & mask);
	clkctrl_dd_recompute(p);
}

/* OSCHFTUNE is a signed 6-bit value; bit 5 is mirrored to bits 7:6 (12.5.8). */
static void
clkctrl_dd_tune_write(struct avr_t * avr, avr_io_addr_t addr, uint8_t v, void * param)
{
	avr_clkctrl_dd_t * p = (avr_clkctrl_dd_t *)param;
	(void)avr; (void)addr;
	if (reg(p, CLKCTRL_DD_OSCHFCTRLA_O) & 0x01)
		return;	/* TUNE is locked while AUTOTUNE is enabled */
	v &= 0x3f;
	if (v & 0x20)
		v |= 0xc0;
	set(p, CLKCTRL_DD_OSCHFTUNE_O, v);
}

/* MCLKINTFLAGS.CFD is write-1-to-clear. */
static void
clkctrl_dd_intflags_write(struct avr_t * avr, avr_io_addr_t addr, uint8_t v, void * param)
{
	avr_clkctrl_dd_t * p = (avr_clkctrl_dd_t *)param;
	(void)avr; (void)addr;
	set(p, CLKCTRL_DD_MCLKINTFLAGS_O, reg(p, CLKCTRL_DD_MCLKINTFLAGS_O) & ~(v & CFD_bm));
}

/* MCLKSTATUS is read-only. */
static void
clkctrl_dd_status_write(struct avr_t * avr, avr_io_addr_t addr, uint8_t v, void * param)
{
	(void)avr; (void)addr; (void)v; (void)param;
}

static void
clkctrl_dd_reset(avr_io_t * io)
{
	avr_clkctrl_dd_t * p = (avr_clkctrl_dd_t *)io;
	avr_t * avr = p->io.avr;

	memset(&avr->data[p->base], 0, 0x21);
	avr->data[p->base + CLKCTRL_DD_OSCHFCTRLA_O] = OSCHF_RESET;

	/* FUSE.OSCCFG.CLKSEL picks the start-up clock: OSCHF (4 MHz) or OSC32K
	 * (DS40002413B 12.3.3 / 8.8.2.3). */
	if (p->osccfg_fuse != 0xff &&
			(avr->fuse[p->osccfg_fuse] & OSCCFG_CLKSEL_gm) == CLKSEL_OSC32K)
		avr->data[p->base + CLKCTRL_DD_MCLKCTRLA_O] = CLKSEL_OSC32K;
	clkctrl_dd_recompute(p);
}

static const char * irq_names[1] = { NULL };

static avr_io_t _io = {
	.kind = "clkctrl_dd",
	.reset = clkctrl_dd_reset,
	.irq_names = irq_names,
};

void
avr_clkctrl_dd_init(
		avr_t * avr,
		avr_clkctrl_dd_t * p,
		avr_io_addr_t base,
		uint8_t osccfg_fuse_index)
{
	static const uint8_t protected_off[] = {
		CLKCTRL_DD_MCLKCTRLA_O, CLKCTRL_DD_MCLKCTRLB_O, CLKCTRL_DD_MCLKCTRLC_O,
		CLKCTRL_DD_MCLKINTCTRL_O, CLKCTRL_DD_OSCHFCTRLA_O, CLKCTRL_DD_PLLCTRLA_O,
		CLKCTRL_DD_OSC32KCTRLA_O, CLKCTRL_DD_XOSC32KCTRLA_O,
		CLKCTRL_DD_XOSCHFCTRLA_O,
	};

	memset(p, 0, sizeof(*p));
	p->io = _io;
	p->base = base;
	p->osccfg_fuse = osccfg_fuse_index;
	p->freq_xosc32k = 32768;

	avr_register_io(avr, &p->io);

	for (unsigned i = 0; i < sizeof(protected_off); i++)
		avr_register_io_write(avr, base + protected_off[i], clkctrl_dd_write, p);
	avr_register_io_write(avr, base + CLKCTRL_DD_OSCHFTUNE_O, clkctrl_dd_tune_write, p);
	avr_register_io_write(avr, base + CLKCTRL_DD_MCLKINTFLAGS_O, clkctrl_dd_intflags_write, p);
	avr_register_io_write(avr, base + CLKCTRL_DD_MCLKSTATUS_O, clkctrl_dd_status_write, p);
}

uint32_t
avr_clkctrl_dd_oschf_hz(avr_clkctrl_dd_t * p)
{
	return frqsel_mhz[(reg(p, CLKCTRL_DD_OSCHFCTRLA_O) & OSCHF_FRQSEL_gm) >>
					  OSCHF_FRQSEL_gp] * 1000000;
}

uint32_t
avr_clkctrl_dd_pll_hz(avr_clkctrl_dd_t * p)
{
	uint8_t pll = reg(p, CLKCTRL_DD_PLLCTRLA_O);
	uint32_t src = (pll & 0x40) ? p->freq_extclk : avr_clkctrl_dd_oschf_hz(p);
	static const uint8_t mul[4] = { 0, 2, 3, 0 };	// DISABLE, 2x, 3x, reserved

	return src * mul[pll & PLL_MULFAC_gm];
}
