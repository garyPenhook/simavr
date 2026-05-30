/*
	avr_bod.c

	"Modern" AVR (AVRxt) Brown-out Detector. See avr_bod.h.

	Models the Voltage Level Monitor (VLM): the supply voltage (VDD, settable in
	millivolts) is compared against a threshold a margin above the BOD level, and
	STATUS.VLMS / INTFLAGS.VLMIF / the BOD_VLM interrupt are driven from a VDD
	crossing in the configured direction. CTRLA (except SLEEP) and CTRLB are
	read-only and loaded from FUSE.BODCFG at reset. The brown-out *reset* effect
	is not modelled.

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
#include "avr_bod.h"

/* CTRLA */
#define SLEEP_gm	0x03
#define ACTIVE_gm	0x0c
#define ACTIVE_gp	2
/* CTRLB */
#define LVL_gm		0x07
/* VLMCTRLA */
#define VLMLVL_gm	0x03
/* INTCTRL */
#define VLMIE_bm	0x01
#define VLMCFG_gm	0x06
#define VLMCFG_gp	1
#define VLMCFG_BELOW	0
#define VLMCFG_ABOVE	1
#define VLMCFG_CROSS	2
/* INTFLAGS / STATUS */
#define VLMIF_bm	0x01
#define VLMS_bm		0x01

static inline uint8_t rd(avr_t *avr, avr_io_addr_t a) { return avr->data[a]; }

/* BOD threshold (mV) for a CTRLB.LVL value: 0->1.8V, 2->2.6V, 7->4.2V. */
static uint32_t bod_lvl_mv(uint8_t lvl)
{
	switch (lvl & LVL_gm) {
	case 0x0: return 1800;
	case 0x2: return 2600;
	case 0x7: return 4200;
	default:  return 1800;	/* reserved levels: treat as the lowest */
	}
}

/* VLM threshold (mV): the BOD level raised by VLMLVL (5 / 15 / 25 %). */
static uint32_t bod_vlm_mv(avr_bod_t *p)
{
	avr_t *avr = p->io.avr;
	uint32_t base = bod_lvl_mv(rd(avr, p->r_ctrlb));
	unsigned pct;
	switch (rd(avr, p->r_vlmctrla) & VLMLVL_gm) {
	case 0x0: pct = 5; break;
	case 0x1: pct = 15; break;
	case 0x2: pct = 25; break;
	default:  pct = 5; break;	/* reserved */
	}
	return base * (100 + pct) / 100;
}

static int bod_enabled(avr_bod_t *p)
{
	return ((rd(p->io.avr, p->r_ctrla) & ACTIVE_gm) >> ACTIVE_gp) != 0;
}

/* Re-evaluate the VLM against the current VDD; update VLMS, flag a crossing. */
static void bod_evaluate(avr_bod_t *p)
{
	avr_t *avr = p->io.avr;
	uint8_t below = p->vdd_mv < bod_vlm_mv(p);

	if (bod_enabled(p)) {
		/* STATUS.VLMS tracks "VDD below threshold" (read-only). */
		uint8_t status = rd(avr, p->r_status) & ~VLMS_bm;
		if (below)
			status |= VLMS_bm;
		avr_core_watch_write(avr, p->r_status, status);

		if (below != p->prev_below) {
			uint8_t cfg = (rd(avr, p->r_intctrl) & VLMCFG_gm) >> VLMCFG_gp;
			int trig = (cfg == VLMCFG_BELOW && below) ||
					   (cfg == VLMCFG_ABOVE && !below) ||
					   (cfg == VLMCFG_CROSS);
			if (trig) {
				avr_core_watch_write(avr, p->r_intflags,
									 rd(avr, p->r_intflags) | VLMIF_bm);
				if (rd(avr, p->r_intctrl) & VLMIE_bm)
					avr_raise_interrupt(avr, &p->vlm_vect);
			}
		}
	}
	/* Track the level even while disabled so enabling later sees no false edge. */
	p->prev_below = below;

	/* Brown-out reset: VDD below the BOD level (CTRLB.LVL) while enabled. Edge-
	 * triggered so it fires once per downward crossing. */
	uint8_t bor = p->vdd_mv < bod_lvl_mv(rd(avr, p->r_ctrlb));
	if (bod_enabled(p) && bor && !p->prev_bor && p->brownout)
		p->brownout(avr, p->brownout_param);
	p->prev_bor = bor;
}

/* CTRLA: only SLEEP[1:0] is writable; ACTIVE/SAMPFREQ are fuse-loaded (R). */
static void
avr_bod_ctrla_write(struct avr_t *avr, avr_io_addr_t addr, uint8_t v, void *param)
{
	(void)param;
	uint8_t res = (rd(avr, addr) & ~SLEEP_gm) | (v & SLEEP_gm);
	avr_core_watch_write(avr, addr, res);
}

/* CTRLB is read-only (fuse-loaded): ignore writes. */
static void
avr_bod_ro_write(struct avr_t *avr, avr_io_addr_t addr, uint8_t v, void *param)
{
	(void)avr; (void)addr; (void)v; (void)param;
}

static void
avr_bod_vlmctrla_write(struct avr_t *avr, avr_io_addr_t addr, uint8_t v, void *param)
{
	avr_bod_t *p = (avr_bod_t *)param;
	avr_core_watch_write(avr, addr, v & VLMLVL_gm);
	bod_evaluate(p);
}

static void
avr_bod_intctrl_write(struct avr_t *avr, avr_io_addr_t addr, uint8_t v, void *param)
{
	avr_bod_t *p = (avr_bod_t *)param;
	avr_core_watch_write(avr, addr, v & (VLMCFG_gm | VLMIE_bm));
	/* Enabling the interrupt while its flag is already set raises immediately. */
	if ((v & VLMIE_bm) && (rd(avr, p->r_intflags) & VLMIF_bm))
		avr_raise_interrupt(avr, &p->vlm_vect);
}

static void
avr_bod_intflags_write(struct avr_t *avr, avr_io_addr_t addr, uint8_t v, void *param)
{
	avr_bod_t *p = (avr_bod_t *)param;
	/* VLMIF is write-1-to-clear. */
	uint8_t res = rd(avr, addr) & ~(v & VLMIF_bm);
	avr_core_watch_write(avr, addr, res);
	if (!(res & VLMIF_bm))
		avr_clear_interrupt(avr, &p->vlm_vect);
}

/* A board/test presents the supply voltage (mV) on the VDD input IRQ. */
static void
avr_bod_irq_vdd(struct avr_irq_t *irq, uint32_t value, void *param)
{
	avr_bod_t *p = (avr_bod_t *)param;
	(void)irq;
	p->vdd_mv = value;
	bod_evaluate(p);
}

static void
avr_bod_reset(avr_io_t *io)
{
	avr_bod_t *p = (avr_bod_t *)io;
	avr_t *avr = p->io.avr;
	uint8_t fuse = (p->bodcfg_fuse_index == 0xff) ? 0 :
				   avr->fuse[p->bodcfg_fuse_index];

	/* CTRLA = SLEEP|ACTIVE|SAMPFREQ (FUSE.BODCFG[4:0]); CTRLB = LVL
	 * (FUSE.BODCFG[7:5]). The VLM registers reset to 0. */
	avr->data[p->r_ctrla] = fuse & 0x1f;
	avr->data[p->r_ctrlb] = (fuse >> 5) & LVL_gm;
	avr->data[p->r_vlmctrla] = 0;
	avr->data[p->r_intctrl] = 0;
	avr->data[p->r_intflags] = 0;
	avr->data[p->r_status] = 0;
	p->prev_below = 0;
	p->prev_bor = 0;
}

static const char *irq_names[AVR_BOD_IRQ_COUNT] = {
	[AVR_BOD_IRQ_VDD_IN] = "bod.vdd",
};

static avr_io_t _io = {
	.kind = "bod",
	.reset = avr_bod_reset,
	.irq_names = irq_names,
};

void
avr_bod_set_vdd(avr_bod_t * p, uint32_t vdd_mv)
{
	p->vdd_mv = vdd_mv;
	bod_evaluate(p);
}

void
avr_bod_set_brownout_handler(avr_bod_t * p,
		void (*cb)(avr_t * avr, void * param), void * param)
{
	p->brownout = cb;
	p->brownout_param = param;
}

void
avr_bod_init(
		avr_t * avr,
		avr_bod_t * p,
		avr_io_addr_t base,
		uint8_t vector,
		uint8_t bodcfg_fuse_index,
		char name)
{
	memset(p, 0, sizeof(*p));
	p->io = _io;
	p->name = name;
	p->base = base;
	p->r_ctrla = base + BODR_CTRLA;
	p->r_ctrlb = base + BODR_CTRLB;
	p->r_vlmctrla = base + BODR_VLMCTRLA;
	p->r_intctrl = base + BODR_INTCTRL;
	p->r_intflags = base + BODR_INTFLAGS;
	p->r_status = base + BODR_STATUS;
	p->bodcfg_fuse_index = bodcfg_fuse_index;
	p->vdd_mv = 3300;

	/* BOD_VLM: enabled by INTCTRL.VLMIE(0), flagged in INTFLAGS.VLMIF(0). */
	p->vlm_vect.vector = vector;
	p->vlm_vect.enable.reg = p->r_intctrl;
	p->vlm_vect.enable.bit = 0;
	p->vlm_vect.enable.mask = 1;
	p->vlm_vect.raised.reg = p->r_intflags;
	p->vlm_vect.raised.bit = 0;
	p->vlm_vect.raised.mask = 1;
	p->vlm_vect.raise_sticky = 1;

	avr_register_io(avr, &p->io);
	avr_register_vector(avr, &p->vlm_vect);

	avr_io_setirqs(&p->io, AVR_IOCTL_BOD_GETIRQ(name), AVR_BOD_IRQ_COUNT, NULL);
	avr_irq_register_notify(p->io.irq + AVR_BOD_IRQ_VDD_IN, avr_bod_irq_vdd, p);

	avr_register_io_write(avr, p->r_ctrla, avr_bod_ctrla_write, p);
	avr_register_io_write(avr, p->r_ctrlb, avr_bod_ro_write, p);
	avr_register_io_write(avr, p->r_vlmctrla, avr_bod_vlmctrla_write, p);
	avr_register_io_write(avr, p->r_intctrl, avr_bod_intctrl_write, p);
	avr_register_io_write(avr, p->r_intflags, avr_bod_intflags_write, p);
	avr_register_io_write(avr, p->r_status, avr_bod_ro_write, p);
}
