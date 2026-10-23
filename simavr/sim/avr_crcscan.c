/*
	avr_crcscan.c

	"Modern" AVR (AVRxt) CRC memory scan. See avr_crcscan.h.

	Enabling CRCSCAN (CTRLA.ENABLE) reports a successful scan immediately:
	STATUS.OK is set and BUSY is left clear, so `while (CRCSCAN.STATUS & BUSY)`
	polls complete and an OK check passes. Setting NMIEN locks CTRLA until reset
	(the device cannot disable a CRC scan that arms the NMI); the RESET strobe
	clears the result and disables the peripheral.

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
#include "avr_crcscan.h"

/* CTRLA */
#define ENABLE_bm	0x01
#define NMIEN_bm	0x02
#define RESET_bm	0x80

/* CTRLB */
#define SRC_gm		0x03
#define SRC_FLASH	0
#define SRC_APP		1	/* boot + application */
#define SRC_BOOT	2
#define SRC_NOCRC	3

/* STATUS */
#define BUSY_bm		0x01
#define OK_bm		0x02

uint16_t
avr_crcscan_crc16(const uint8_t *data, uint32_t len)
{
	uint16_t crc = 0xffff;
	for (uint32_t i = 0; i < len; i++) {
		crc ^= (uint16_t)data[i] << 8;
		for (int b = 0; b < 8; b++)
			crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
	}
	return crc;
}

/* CRC-32 (IEEE 802.3 polynomial), bytes shifted in MSB first as 30.3.2 describes.
 * DS40002413B gives the start value only for CRC16 (0xFFFF); 0xFFFFFFFF by
 * analogy, no final XOR: UNVERIFIED against silicon. */
uint32_t
avr_crcscan_crc32(const uint8_t *data, uint32_t len)
{
	uint32_t crc = 0xffffffffu;
	for (uint32_t i = 0; i < len; i++) {
		crc ^= (uint32_t)data[i] << 24;
		for (int b = 0; b < 8; b++)
			crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04c11db7u : (crc << 1);
	}
	return crc;
}

/* Last byte index of the section selected by CTRLB.SRC. */
static uint32_t crcscan_section_end(avr_crcscan_t *p, uint8_t src)
{
	avr_t *avr = p->io.avr;
	uint32_t end = p->flash_size - 1;	/* full flash */

	uint32_t unit = p->dd ? 512 : 256;

	if (src == SRC_APP && p->append_idx != 0xff && avr->fuse[p->append_idx])
		end = (uint32_t)avr->fuse[p->append_idx] * unit - 1;
	else if (src == SRC_BOOT && p->bootend_idx != 0xff && avr->fuse[p->bootend_idx])
		end = (uint32_t)avr->fuse[p->bootend_idx] * unit - 1;

	if (end >= p->flash_size)
		end = p->flash_size - 1;
	return end;
}

/* Boot-time CRCSRC comes from FUSE.SYSCFG0[7:6]. For compatibility with older
 * tests/boards that never populated modern fuse defaults, an all-zero SYSCFG0
 * is treated as "unspecified" and leaves startup CRC disabled. */
static uint8_t crcscan_boot_src(avr_crcscan_t *p)
{
	avr_t *avr = p->io.avr;

	if (p->syscfg0_idx == 0xff)
		return SRC_NOCRC;
	if (p->dd) {
		/* FUSE.SYSCFG0.CRCSRC: FLASH, BOOT, BOOTAPP, NOCRC (DS40002413B 8.8.2.4). */
		static const uint8_t map[4] = { SRC_FLASH, SRC_BOOT, SRC_APP, SRC_NOCRC };
		return map[(avr->fuse[p->syscfg0_idx] >> 6) & SRC_gm];
	}
	if (avr->fuse[p->syscfg0_idx] == 0x00)
		return SRC_NOCRC;
	return (avr->fuse[p->syscfg0_idx] >> 6) & SRC_gm;
}

/* Run the scan: CRC the section (minus its 2 checksum bytes) and compare with
 * the stored big-endian checksum. OK on match; on mismatch clear OK and, if
 * NMIEN, raise the NMI. */
static int crcscan_scan(avr_crcscan_t *p, uint8_t src, int raise_nmi)
{
	avr_t *avr = p->io.avr;
	uint32_t end = crcscan_section_end(p, src);

	int ok = 0;
	if (p->dd && p->syscfg0_idx != 0xff && (avr->fuse[p->syscfg0_idx] & 0x20)) {
		/* FUSE.SYSCFG0.CRCSEL = CRC32; checksum in the last 4 bytes, MSB at
		 * the highest address (Table 30-2). */
		if (end >= 4) {
			uint32_t stored = avr->flash[end - 3] | (avr->flash[end - 2] << 8) |
					((uint32_t)avr->flash[end - 1] << 16) | ((uint32_t)avr->flash[end] << 24);
			ok = (avr_crcscan_crc32(avr->flash, end - 3) == stored);
		}
	} else if (end >= 2) {
		uint16_t stored = (avr->flash[end - 1] << 8) | avr->flash[end];
		uint16_t crc = avr_crcscan_crc16(avr->flash, end - 1);
		ok = (crc == stored);
	}

	avr_core_watch_write(avr, p->r_status, ok ? OK_bm : 0);
	if (!ok && raise_nmi && (avr->data[p->r_ctrla] & NMIEN_bm))
		avr_raise_interrupt(avr, &p->nmi);
	return ok;
}

static void
avr_crcscan_ctrla_write(struct avr_t *avr, avr_io_addr_t addr, uint8_t v,
						void *param)
{
	avr_crcscan_t *p = (avr_crcscan_t *)param;

	/* Once the NMI is armed CTRLA is read-only until reset. */
	if (p->locked)
		return;

	/* RESET strobe: clear the result and disable; the bit self-clears. */
	if (v & RESET_bm) {
		avr_core_watch_write(avr, p->r_ctrla, 0);
		avr_core_watch_write(avr, p->r_status, 0);
		return;
	}

	avr_core_watch_write(avr, addr, v);

	if (v & ENABLE_bm) {
		if (v & NMIEN_bm)
			p->locked = 1;
		crcscan_scan(p, avr->data[p->r_ctrlb] & SRC_gm, 1);
		/* instant: OK/fail set now, never BUSY */
	} else {
		avr_core_watch_write(avr, p->r_status, 0);
	}
}

static void
avr_crcscan_reset(avr_io_t *io)
{
	avr_crcscan_t *p = (avr_crcscan_t *)io;
	uint8_t src = crcscan_boot_src(p);
	p->locked = 0;
	p->io.avr->data[p->r_status] = 0;
	if (src != SRC_NOCRC && !crcscan_scan(p, src, 0))
		p->io.avr->state = cpu_Stopped;
}

static const char *irq_names[1] = { NULL };

static avr_io_t _io = {
	.kind = "crcscan",
	.reset = avr_crcscan_reset,
	.irq_names = irq_names,
};

void
avr_crcscan_init(
		avr_t * avr,
		avr_crcscan_t * p,
		avr_io_addr_t base,
		uint32_t flash_size,
		uint8_t syscfg0_fuse_index,
		uint8_t append_fuse_index,
		uint8_t bootend_fuse_index,
		uint8_t nmi_vector,
		char name)
{
	memset(p, 0, sizeof(*p));
	p->io = _io;
	p->name = name;
	p->base = base;
	p->r_ctrla = base + CRCSCANR_CTRLA;
	p->r_ctrlb = base + CRCSCANR_CTRLB;
	p->r_status = base + CRCSCANR_STATUS;
	p->flash_size = flash_size;
	p->syscfg0_idx = syscfg0_fuse_index;
	p->append_idx = append_fuse_index;
	p->bootend_idx = bootend_fuse_index;

	/* CRC-failure NMI: non-maskable, armed by CTRLA.NMIEN(1), sticky. */
	p->nmi.vector = nmi_vector;
	p->nmi.nmi = 1;
	p->nmi.enable.reg = p->r_ctrla;
	p->nmi.enable.bit = 1;		/* NMIEN */
	p->nmi.enable.mask = 1;
	p->nmi.raise_sticky = 1;

	avr_register_io(avr, &p->io);
	avr_register_vector(avr, &p->nmi);
	avr_register_io_write(avr, p->r_ctrla, avr_crcscan_ctrla_write, p);
}

void
avr_crcscan_init_dd(
		avr_t * avr,
		avr_crcscan_t * p,
		avr_io_addr_t base,
		uint32_t flash_size,
		uint8_t syscfg0_fuse_index,
		uint8_t codesize_fuse_index,
		uint8_t bootsize_fuse_index,
		uint8_t nmi_vector,
		char name)
{
	avr_crcscan_init(avr, p, base, flash_size, syscfg0_fuse_index,
					 codesize_fuse_index, bootsize_fuse_index, nmi_vector, name);
	p->dd = 1;
}
