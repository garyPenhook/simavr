/*
	avr_nvmctrl.c

	"Modern" AVR (AVRxt) Non-Volatile Memory Controller (NVMCTRL). See
	avr_nvmctrl.h.

	EEPROM model
	------------
	The committed EEPROM lives in avr->data[ee_start..] (so it is read back with
	ordinary loads from the mapped address). Writes to the mapped region do NOT
	land there directly; they accumulate in a page buffer with a per-byte dirty
	mask. A command written to NVMCTRL.CTRLA (only honoured inside a CCP window)
	then acts on it:
	  PAGEWRITE / PAGEERASEWRITE  commit the dirty buffer bytes to EEPROM
	  PAGEERASE                   set the dirty bytes to 0xFF
	  PAGEBUFCLR                  discard the buffer
	  EEERASE / CHIPERASE         erase the whole EEPROM to 0xFF
	Completion sets INTFLAGS.EEREADY and raises NVMCTRL_EE if enabled. Commands
	complete instantly (STATUS busy flags are never observed set). Flash
	self-programming is not modelled (writes to mapped flash are ignored by the
	engine).

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
#include "avr_nvmctrl.h"

/* CTRLA command field */
#define CMD_gm		0x07
#define CMD_NONE	0
#define CMD_PAGEWRITE	1
#define CMD_PAGEERASE	2
#define CMD_PAGEERASEWRITE 3
#define CMD_PAGEBUFCLR	4
#define CMD_CHIPERASE	5
#define CMD_EEERASE	6
#define CMD_FUSEWRITE	7

/* STATUS */
#define FBUSY_bm	0x01
#define EEBUSY_bm	0x02
#define WRERROR_bm	0x04

/* INTCTRL / INTFLAGS */
#define EEREADY_bm	0x01

static void nvm_bufclr(avr_nvmctrl_t *p)
{
	memset(p->dirty, 0, p->ee_size);
}

/* Mark a command complete: not busy, EEPROM ready, raise interrupt if enabled. */
static void nvm_complete(avr_nvmctrl_t *p)
{
	avr_t *avr = p->io.avr;
	avr_core_watch_write(avr, p->r_status,
			avr->data[p->r_status] & ~(EEBUSY_bm | FBUSY_bm));
	avr_core_watch_write(avr, p->r_intflags,
			avr->data[p->r_intflags] | EEREADY_bm);
	if (avr->data[p->r_intctrl] & EEREADY_bm)
		avr_raise_interrupt(avr, &p->eeready);
}

/* A byte written to the mapped EEPROM region: load the page buffer. */
static void
avr_nvmctrl_ee_write(struct avr_t *avr, avr_io_addr_t addr,
					 uint8_t v, void *param)
{
	avr_nvmctrl_t *p = (avr_nvmctrl_t *)param;
	uint16_t off = addr - p->ee_start;
	if (off >= p->ee_size)
		return;
	p->buf[off] = v;
	p->dirty[off] = 1;
	/* note: avr->data[addr] (committed EEPROM) is intentionally left unchanged */
}

static void
avr_nvmctrl_ctrla_write(struct avr_t *avr, avr_io_addr_t addr,
						uint8_t v, void *param)
{
	avr_nvmctrl_t *p = (avr_nvmctrl_t *)param;
	uint8_t cmd = v & CMD_gm;

	/* CTRLA command is configuration-change protected. */
	if (!avr_ccp_io_write_enabled(avr))
		return;
	avr_core_watch_write(avr, p->r_ctrla, v);

	switch (cmd) {
	case CMD_PAGEWRITE:
	case CMD_PAGEERASEWRITE:
		for (uint16_t i = 0; i < p->ee_size; i++)
			if (p->dirty[i])
				avr_core_watch_write(avr, p->ee_start + i, p->buf[i]);
		nvm_bufclr(p);
		nvm_complete(p);
		break;
	case CMD_PAGEERASE:
		for (uint16_t i = 0; i < p->ee_size; i++)
			if (p->dirty[i])
				avr_core_watch_write(avr, p->ee_start + i, 0xff);
		nvm_bufclr(p);
		nvm_complete(p);
		break;
	case CMD_PAGEBUFCLR:
		nvm_bufclr(p);
		nvm_complete(p);
		break;
	case CMD_EEERASE:
	case CMD_CHIPERASE:
		for (uint16_t i = 0; i < p->ee_size; i++)
			avr_core_watch_write(avr, p->ee_start + i, 0xff);
		nvm_bufclr(p);
		nvm_complete(p);
		break;
	default:
		break;
	}
}

static void
avr_nvmctrl_intflags_write(struct avr_t *avr, avr_io_addr_t addr,
						   uint8_t v, void *param)
{
	avr_nvmctrl_t *p = (avr_nvmctrl_t *)param;
	uint8_t res = avr->data[p->r_intflags] & ~v;	/* write-1-to-clear */
	avr_core_watch_write(avr, p->r_intflags, res);
	if (!(res & EEREADY_bm))
		avr_clear_interrupt(avr, &p->eeready);
}

static void
avr_nvmctrl_reset(avr_io_t *io)
{
	avr_nvmctrl_t *p = (avr_nvmctrl_t *)io;
	avr_t *avr = p->io.avr;
	nvm_bufclr(p);
	/* Erased EEPROM reads as 0xFF unless something loaded it. */
	for (uint16_t i = 0; i < p->ee_size; i++)
		if (avr->data[p->ee_start + i] == 0)
			avr->data[p->ee_start + i] = 0xff;
	avr->data[p->r_status] = 0;
}

static const char *irq_names[1] = { NULL };

static avr_io_t _io = {
	.kind = "nvmctrl",
	.reset = avr_nvmctrl_reset,
	.irq_names = irq_names,
};

void
avr_nvmctrl_init(
		avr_t * avr,
		avr_nvmctrl_t * p,
		avr_io_addr_t base,
		avr_io_addr_t ee_start,
		uint16_t ee_size,
		uint8_t vec_eeready)
{
	memset(p, 0, sizeof(*p));
	p->io = _io;
	p->base = base;
	p->r_ctrla = base + NVMR_CTRLA;
	p->r_status = base + NVMR_STATUS;
	p->r_intctrl = base + NVMR_INTCTRL;
	p->r_intflags = base + NVMR_INTFLAGS;
	p->ee_start = ee_start;
	p->ee_size = ee_size > AVR_NVM_EE_MAX ? AVR_NVM_EE_MAX : ee_size;

	p->eeready.vector = vec_eeready;
	p->eeready.enable.reg = p->r_intctrl;  p->eeready.enable.bit = 0;
	p->eeready.enable.mask = 1;
	p->eeready.raised.reg = p->r_intflags; p->eeready.raised.bit = 0;
	p->eeready.raised.mask = 1;
	p->eeready.raise_sticky = 1;

	avr_register_io(avr, &p->io);
	avr_register_vector(avr, &p->eeready);

	avr_register_io_write(avr, p->r_ctrla, avr_nvmctrl_ctrla_write, p);
	avr_register_io_write(avr, p->r_intflags, avr_nvmctrl_intflags_write, p);

	/* Intercept writes to the mapped EEPROM region (page-buffer load). */
	for (uint16_t i = 0; i < p->ee_size; i++)
		avr_register_io_write(avr, ee_start + i, avr_nvmctrl_ee_write, p);
}
