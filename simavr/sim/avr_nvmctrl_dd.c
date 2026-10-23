/*
	avr_nvmctrl_dd.c

	AVR DD Nonvolatile Memory Controller. See avr_nvmctrl_dd.h.

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
#include "avr_nvmctrl_dd.h"
#include "sim_cycle_timers.h"

/* Nominal NVM operation duration (see avr_nvmctrl.c). */
#define NVM_OP_CYCLES	100

/* CTRLA.CMD (DS40002413B 11.5.1) */
#define CMD_gm		0x7f
#define CMD_NOCMD	0x00
#define CMD_NOOP	0x01
#define CMD_FLWR	0x02
#define CMD_FLPER	0x08
#define CMD_FLMPER2	0x09
#define CMD_FLMPER32 0x0d
#define CMD_EEWR	0x12
#define CMD_EEERWR	0x13
#define CMD_EEBER	0x18
#define CMD_EEMBER2	0x19
#define CMD_EEMBER32 0x1d
#define CMD_CHER	0x20
#define CMD_EECHER	0x30

/* CTRLB */
#define FLMAPLOCK_bm	0x80
#define FLMAP_gm		0x30
#define APPDATAWP_bm	0x04
#define BOOTRP_bm		0x02
#define APPCODEWP_bm	0x01
#define CTRLB_RESET		0x30

/* STATUS */
#define FBUSY_bm		0x01
#define EEBUSY_bm		0x02
#define ERROR_gm		0x70
#define ERROR_gp		4
#define ERR_INVALIDCMD	1
#define ERR_WRITEPROTECT 2
#define ERR_CMDCOLLISION 3

/* INTCTRL / INTFLAGS */
#define EEREADY_bm		0x01

enum { SEC_BOOT, SEC_APPCODE, SEC_APPDATA };

static inline uint8_t
reg(avr_nvmctrl_dd_t * p, int off)
{
	return p->io.avr->data[p->base + off];
}

static inline void
set(avr_nvmctrl_dd_t * p, int off, uint8_t v)
{
	avr_core_watch_write(p->io.avr, p->base + off, v);
}

static void
nvm_error(avr_nvmctrl_dd_t * p, int code)
{
	set(p, NVMDD_STATUS, (reg(p, NVMDD_STATUS) & ~ERROR_gm) | (code << ERROR_gp));
}

/* Remember the last accessed location in ADDR and the data in DATA (11.5.6/7). */
static void
nvm_latch(avr_nvmctrl_dd_t * p, uint32_t addr, uint8_t data)
{
	set(p, NVMDD_ADDR, addr);
	set(p, NVMDD_ADDR + 1, addr >> 8);
	set(p, NVMDD_ADDR + 2, addr >> 16);
	set(p, NVMDD_DATA, data);
}

static avr_cycle_count_t
nvm_op_done(struct avr_t * avr, avr_cycle_count_t when, void * param)
{
	avr_nvmctrl_dd_t * p = (avr_nvmctrl_dd_t *)param;
	(void)when;
	uint8_t st = reg(p, NVMDD_STATUS);

	if (st & EEBUSY_bm) {
		/* EEREADY is set continuously while the EEPROM is not busy (11.5.5). */
		set(p, NVMDD_INTFLAGS, reg(p, NVMDD_INTFLAGS) | EEREADY_bm);
		if (reg(p, NVMDD_INTCTRL) & EEREADY_bm)
			avr_raise_interrupt(avr, &p->eeready);
	}
	set(p, NVMDD_STATUS, st & ~(EEBUSY_bm | FBUSY_bm));
	return 0;
}

static void
nvm_begin(avr_nvmctrl_dd_t * p, uint8_t busy_bm)
{
	avr_t * avr = p->io.avr;
	if (busy_bm & EEBUSY_bm) {
		set(p, NVMDD_INTFLAGS, reg(p, NVMDD_INTFLAGS) & ~EEREADY_bm);
		avr_clear_interrupt(avr, &p->eeready);
	}
	set(p, NVMDD_STATUS, reg(p, NVMDD_STATUS) | busy_bm);
	avr_cycle_timer_register(avr, NVM_OP_CYCLES, nvm_op_done, p);
}

static int
nvm_busy(avr_nvmctrl_dd_t * p)
{
	return (reg(p, NVMDD_STATUS) & (FBUSY_bm | EEBUSY_bm)) != 0;
}

/*
 * Flash sections (DS40002413B 11.3.1.1, Table 11-2): FUSE.BOOTSIZE and
 * FUSE.CODESIZE are in 512-byte blocks. BOOTSIZE = 0 makes the whole flash BOOT;
 * a value that exceeds the flash is ignored and the default (0) used.
 */
static int
nvm_section(avr_nvmctrl_dd_t * p, uint32_t byte_addr)
{
	avr_t * avr = p->io.avr;
	uint32_t boot = (uint32_t)avr->fuse[p->fuse_bootsize] * 512;
	uint32_t code = (uint32_t)avr->fuse[p->fuse_codesize] * 512;

	if (boot > p->flash_size)
		boot = 0;
	if (code > p->flash_size)
		code = 0;
	if (!boot || byte_addr < boot)
		return SEC_BOOT;
	if (!code)
		return SEC_APPCODE;			// BOOTEND to FLASHEND, no APPDATA
	if (code <= boot)
		return SEC_APPDATA;			// APPCODE removed
	return byte_addr < code ? SEC_APPCODE : SEC_APPDATA;
}

/* Table 11-1: BOOT is never writable; APPCODE only from BOOT; APPDATA from BOOT
 * or APPCODE. APPCODEWP / APPDATAWP additionally lock their section. */
static int
nvm_flash_write_allowed(avr_nvmctrl_dd_t * p, uint32_t target)
{
	int exec = nvm_section(p, p->io.avr->pc);
	int sec = nvm_section(p, target);
	uint8_t ctrlb = reg(p, NVMDD_CTRLB);

	switch (sec) {
	case SEC_BOOT:
		return 0;
	case SEC_APPCODE:
		return exec == SEC_BOOT && !(ctrlb & APPCODEWP_bm);
	default:
		return exec != SEC_APPDATA && !(ctrlb & APPDATAWP_bm);
	}
}

static int
cmd_is_flash(uint8_t cmd)
{
	return cmd == CMD_FLWR || (cmd >= CMD_FLPER && cmd <= CMD_FLMPER32);
}

static int
cmd_is_ee(uint8_t cmd)
{
	return cmd == CMD_EEWR || cmd == CMD_EEERWR ||
			(cmd >= CMD_EEBER && cmd <= CMD_EEMBER32);
}

/* Number of pages (flash) or bytes (EEPROM) erased by an FLMPERn / EEMBERn. */
static unsigned
cmd_multi_count(uint8_t cmd, uint8_t first)
{
	return 1u << (cmd - first + 1);
}

/*
 * A store to the data-space flash mapping (avr->flashmap_write). A store that
 * arrives while the array is still busy makes the real CPU wait (11.3.2.1,
 * 11.3.2.3.4), so it is simply applied here; only a new *command* written while
 * busy is a collision.
 */
static void
nvm_flash_store(struct avr_t * avr, uint16_t addr, uint8_t v, void * param)
{
	avr_nvmctrl_dd_t * p = (avr_nvmctrl_dd_t *)param;
	uint8_t cmd = reg(p, NVMDD_CTRLA) & CMD_gm;
	uint32_t off = (uint32_t)addr - p->flash_start;

	if (off >= p->flash_size || !cmd_is_flash(cmd))
		return;		// no flash write/erase mode armed: the store is ignored
	if (!nvm_flash_write_allowed(p, off)) {
		nvm_error(p, ERR_WRITEPROTECT);
		return;
	}
	nvm_latch(p, off, v);
	if (cmd == CMD_FLWR) {
		avr->flash[off] = v;
	} else {
		unsigned pages = (cmd == CMD_FLPER) ? 1 : cmd_multi_count(cmd, CMD_FLMPER2);
		uint32_t first = (off / p->flash_page) & ~(pages - 1);
		/* Check every page before erasing any: a protected page aborts the lot. */
		for (unsigned pg = 0; pg < pages; pg++) {
			uint32_t o = (first + pg) * p->flash_page;
			if (o + p->flash_page <= p->flash_size &&
					!nvm_flash_write_allowed(p, o)) {
				nvm_error(p, ERR_WRITEPROTECT);
				return;
			}
		}
		for (unsigned pg = 0; pg < pages; pg++) {
			uint32_t o = (first + pg) * p->flash_page;
			if (o + p->flash_page <= p->flash_size)
				memset(&avr->flash[o], 0xff, p->flash_page);
		}
	}
	nvm_begin(p, FBUSY_bm);
}

/* A store to the memory-mapped EEPROM. */
static void
nvm_ee_store(struct avr_t * avr, avr_io_addr_t addr, uint8_t v, void * param)
{
	avr_nvmctrl_dd_t * p = (avr_nvmctrl_dd_t *)param;
	uint8_t cmd = reg(p, NVMDD_CTRLA) & CMD_gm;
	uint16_t off = addr - p->ee_start;

	if (!cmd_is_ee(cmd))
		return;
	nvm_latch(p, off, v);
	switch (cmd) {
	case CMD_EEWR:
	case CMD_EEERWR:
		avr->data[addr] = v;
		break;
	case CMD_EEBER:
		avr->data[addr] = 0xff;		// always reads back 0xFF (11.3.2.3.6)
		break;
	default: {
		unsigned n = cmd_multi_count(cmd, CMD_EEMBER2);
		unsigned first = off & ~(n - 1);
		memset(&avr->data[p->ee_start + first], 0xff, n);
	}	break;
	}
	nvm_begin(p, EEBUSY_bm);
}

/* The User Row is erased/written like flash: FLPER erases the whole row, FLWR
 * writes a byte (11.3.1.4, 11.3.2.2). */
static void
nvm_uro_store(struct avr_t * avr, avr_io_addr_t addr, uint8_t v, void * param)
{
	avr_nvmctrl_dd_t * p = (avr_nvmctrl_dd_t *)param;
	uint8_t cmd = reg(p, NVMDD_CTRLA) & CMD_gm;

	if (cmd != CMD_FLWR && cmd != CMD_FLPER)
		return;
	nvm_latch(p, addr - p->uro_start, v);
	if (cmd == CMD_FLWR)
		avr->data[addr] = v;
	else
		memset(&avr->data[p->uro_start], 0xff, p->uro_size);
	nvm_begin(p, FBUSY_bm);
}

static int
cmd_valid(uint8_t cmd)
{
	switch (cmd) {
	case CMD_NOCMD: case CMD_NOOP: case CMD_FLWR: case CMD_EEWR: case CMD_EEERWR:
	case CMD_EEBER: case CMD_CHER: case CMD_EECHER:
		return 1;
	}
	return (cmd >= CMD_FLPER && cmd <= CMD_FLMPER32) ||
			(cmd >= CMD_EEMBER2 && cmd <= CMD_EEMBER32);
}

static void
nvm_ctrla_write(struct avr_t * avr, avr_io_addr_t addr, uint8_t v, void * param)
{
	avr_nvmctrl_dd_t * p = (avr_nvmctrl_dd_t *)param;
	uint8_t cmd = v & CMD_gm;
	(void)addr;

	if (!avr_ccp_io_write_enabled(avr))
		return;		// CTRLA is Configuration Change Protected
	if (!cmd_valid(cmd)) {
		nvm_error(p, ERR_INVALIDCMD);
		return;
	}
	if (cmd == CMD_CHER)
		return;		// only the UPDI can start a chip erase (11.5.1)
	if (cmd != CMD_NOCMD && cmd != CMD_NOOP && nvm_busy(p)) {
		nvm_error(p, ERR_CMDCOLLISION);
		return;
	}
	set(p, NVMDD_CTRLA, cmd);
	if (cmd == CMD_EECHER) {
		memset(&avr->data[p->ee_start], 0xff, p->ee_size);
		nvm_begin(p, EEBUSY_bm);
		set(p, NVMDD_CTRLA, CMD_NOCMD);		// a one-shot command, nothing stays armed
	}
}

/*
 * CTRLB (DS40002413B 11.5.2): FLMAP[1:0] is not under CCP and is frozen by
 * FLMAPLOCK. The write-protect bits and FLMAPLOCK are CCP protected and, once
 * set, only a Reset clears them.
 */
static void
nvm_ctrlb_write(struct avr_t * avr, avr_io_addr_t addr, uint8_t v, void * param)
{
	avr_nvmctrl_dd_t * p = (avr_nvmctrl_dd_t *)param;
	uint8_t old = reg(p, NVMDD_CTRLB);
	uint8_t nv = old;
	(void)addr;

	if (!(old & FLMAPLOCK_bm))
		nv = (nv & ~FLMAP_gm) | (v & FLMAP_gm);
	if (avr_ccp_io_write_enabled(avr)) {
		nv |= v & (FLMAPLOCK_bm | APPDATAWP_bm | APPCODEWP_bm);
		/* BOOTRP may only be written from the BOOT section. */
		if (nvm_section(p, avr->pc) == SEC_BOOT)
			nv |= v & BOOTRP_bm;
	}
	set(p, NVMDD_CTRLB, nv);
}

/* STATUS: ERROR[2:0] can only be cleared (write 0), EEBUSY/FBUSY are read-only. */
static void
nvm_status_write(struct avr_t * avr, avr_io_addr_t addr, uint8_t v, void * param)
{
	avr_nvmctrl_dd_t * p = (avr_nvmctrl_dd_t *)param;
	uint8_t st = reg(p, NVMDD_STATUS);
	(void)avr; (void)addr;
	set(p, NVMDD_STATUS, (st & ~ERROR_gm) | (st & v & ERROR_gm));
}

/* INTFLAGS.EEREADY is set while the EEPROM is idle and cleared by writing 1; a
 * clear while idle therefore has no lasting effect (11.5.5). */
static void
nvm_intflags_write(struct avr_t * avr, avr_io_addr_t addr, uint8_t v, void * param)
{
	avr_nvmctrl_dd_t * p = (avr_nvmctrl_dd_t *)param;
	(void)avr; (void)addr;
	if ((v & EEREADY_bm) && (reg(p, NVMDD_STATUS) & EEBUSY_bm))
		set(p, NVMDD_INTFLAGS, reg(p, NVMDD_INTFLAGS) & ~EEREADY_bm);
}

/* EEREADY is a level interrupt: enabling it while the EEPROM is idle fires. */
static void
nvm_intctrl_write(struct avr_t * avr, avr_io_addr_t addr, uint8_t v, void * param)
{
	avr_nvmctrl_dd_t * p = (avr_nvmctrl_dd_t *)param;
	(void)addr;
	set(p, NVMDD_INTCTRL, v & EEREADY_bm);
	if (!(v & EEREADY_bm))
		avr_clear_interrupt(avr, &p->eeready);
	if ((v & EEREADY_bm) && (reg(p, NVMDD_INTFLAGS) & EEREADY_bm))
		avr_raise_interrupt(avr, &p->eeready);
}

static void
nvm_reset(avr_io_t * io)
{
	avr_nvmctrl_dd_t * p = (avr_nvmctrl_dd_t *)io;
	avr_t * avr = p->io.avr;

	avr_cycle_timer_cancel(avr, nvm_op_done, p);
	memset(&avr->data[p->base], 0, 0x0b);
	avr->data[p->base + NVMDD_CTRLB] = CTRLB_RESET;		// FLMAP = 3 (11.5.2)
	avr->data[p->base + NVMDD_INTFLAGS] = EEREADY_bm;	// EEPROM idle
	/* EEPROM and USERROW are non-volatile: left untouched (arch.persist_*). */
}

static const char * irq_names[1] = { NULL };

static avr_io_t _io = {
	.kind = "nvmctrl_dd",
	.reset = nvm_reset,
	.irq_names = irq_names,
};

void
avr_nvmctrl_dd_init(
		avr_t * avr,
		avr_nvmctrl_dd_t * p,
		avr_io_addr_t base,
		avr_io_addr_t ee_start, uint16_t ee_size,
		avr_io_addr_t uro_start, uint16_t uro_size,
		avr_io_addr_t flash_start, uint32_t flash_size, uint16_t flash_page,
		uint8_t fuse_codesize_index, uint8_t fuse_bootsize_index,
		uint8_t vec_eeready)
{
	memset(p, 0, sizeof(*p));
	p->io = _io;
	p->base = base;
	p->ee_start = ee_start;
	p->ee_size = ee_size;
	p->uro_start = uro_start;
	p->uro_size = uro_size;
	p->flash_start = flash_start;
	p->flash_size = flash_size;
	p->flash_page = flash_page;
	p->fuse_codesize = fuse_codesize_index;
	p->fuse_bootsize = fuse_bootsize_index;

	/* EEPROM and USERROW persist across reset and start erased. */
	avr->arch.persist_start = ee_start;
	avr->arch.persist_end = ee_start + ee_size - 1;
	memset(&avr->data[ee_start], 0xff, ee_size);
	avr->arch.persist2_start = uro_start;
	avr->arch.persist2_end = uro_start + uro_size - 1;
	memset(&avr->data[uro_start], 0xff, uro_size);

	p->eeready.vector = vec_eeready;
	p->eeready.enable.reg = base + NVMDD_INTCTRL;	p->eeready.enable.mask = 1;
	p->eeready.raised.reg = base + NVMDD_INTFLAGS;	p->eeready.raised.mask = 1;
	p->eeready.raise_sticky = 1;	// level interrupt: the flag outlives the ISR

	avr_register_io(avr, &p->io);
	avr_register_vector(avr, &p->eeready);

	avr_register_io_write(avr, base + NVMDD_CTRLA, nvm_ctrla_write, p);
	avr_register_io_write(avr, base + NVMDD_CTRLB, nvm_ctrlb_write, p);
	avr_register_io_write(avr, base + NVMDD_STATUS, nvm_status_write, p);
	avr_register_io_write(avr, base + NVMDD_INTCTRL, nvm_intctrl_write, p);
	avr_register_io_write(avr, base + NVMDD_INTFLAGS, nvm_intflags_write, p);

	for (uint16_t i = 0; i < ee_size; i++)
		avr_register_io_write(avr, ee_start + i, nvm_ee_store, p);
	for (uint16_t i = 0; i < uro_size; i++)
		avr_register_io_write(avr, uro_start + i, nvm_uro_store, p);

	/* The engine forwards stores to the data-space flash mapping to us. */
	avr->flashmap_write = nvm_flash_store;
	avr->flashmap_write_param = p;
}
