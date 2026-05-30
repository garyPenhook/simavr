/*
	avr_nvmctrl.c

	Modern AVR NVMCTRL (EEPROM path). See avr_nvmctrl.h.

	Copyright 2024 simavr modern-AVR fork (garyPenhook)

	This file is part of simavr. GNU GPL v3 or later; see COPYING.
 */

#include <stdlib.h>
#include <string.h>
#include "avr_nvmctrl.h"

// a write to the mapped EEPROM region loads the page buffer (it does not
// change the stored EEPROM until a commit command is issued)
static void
avr_nvm_eeprom_write(avr_t * avr, avr_io_addr_t addr, uint8_t v, void * param)
{
	avr_nvmctrl_t * p = (avr_nvmctrl_t *)param;
	uint16_t off = addr - p->eeprom_base;
	if (off >= p->eeprom_size)
		return;
	p->page_buf[off] = v;
	p->page_dirty[off] = 1;
	// data[] (the mapped-read view) is intentionally NOT updated yet
}

// commit / erase according to the command written to CTRLA
static void
avr_nvm_ctrla_write(avr_t * avr, avr_io_addr_t addr, uint8_t v, void * param)
{
	avr_nvmctrl_t * p = (avr_nvmctrl_t *)param;
	uint8_t cmd = v & AVR_NVM_CMD_CMD_gm;
	avr->data[addr] = v;

	switch (cmd) {
		case AVR_NVM_CMD_PAGEWRITE:
		case AVR_NVM_CMD_PAGEERASEWRITE:
			for (uint16_t i = 0; i < p->eeprom_size; i++)
				if (p->page_dirty[i]) {
					p->eeprom[i] = p->page_buf[i];
					avr->data[p->eeprom_base + i] = p->page_buf[i];
					p->page_dirty[i] = 0;
				}
			break;
		case AVR_NVM_CMD_PAGEERASE:
			for (uint16_t i = 0; i < p->eeprom_size; i++)
				if (p->page_dirty[i]) {
					p->eeprom[i] = 0xFF;
					avr->data[p->eeprom_base + i] = 0xFF;
					p->page_dirty[i] = 0;
				}
			break;
		case AVR_NVM_CMD_EEERASE:
			memset(p->eeprom, 0xFF, p->eeprom_size);
			memset(p->page_dirty, 0, p->eeprom_size);
			for (uint16_t i = 0; i < p->eeprom_size; i++)
				avr->data[p->eeprom_base + i] = 0xFF;
			break;
		case AVR_NVM_CMD_PAGEBUFCLR:
			memset(p->page_dirty, 0, p->eeprom_size);
			break;
		default:
			break;
	}
	// the operation completes immediately in simulation
	avr->data[p->r_base + AVR_NVM_INTFLAGS] |= AVR_NVM_EEREADY;
	if (p->eeready.vector)
		avr_raise_interrupt(avr, &p->eeready);
}

// EEPROM/flash are never busy in simulation
static uint8_t
avr_nvm_status_read(avr_t * avr, avr_io_addr_t addr, void * param)
{
	return 0;
}

static void
avr_nvm_reset(avr_io_t * io)
{
	avr_nvmctrl_t * p = (avr_nvmctrl_t *)io;
	avr_t * avr = p->io.avr;

	memset(p->page_dirty, 0, p->eeprom_size);
	// avr_reset() has just zeroed the data space; restore the mapped EEPROM
	// view from the persistent contents so reads work after reset.
	for (uint16_t i = 0; i < p->eeprom_size; i++)
		avr->data[p->eeprom_base + i] = p->eeprom[i];
	avr->data[p->r_base + AVR_NVM_INTFLAGS] |= AVR_NVM_EEREADY;
}

void
avr_nvmctrl_init(avr_t * avr, avr_nvmctrl_t * p)
{
	p->io.kind = "nvmctrl";
	p->io.reset = avr_nvm_reset;
	avr_register_io(avr, &p->io);
	if (p->eeready.vector)
		avr_register_vector(avr, &p->eeready);

	p->eeprom = malloc(p->eeprom_size);
	p->page_buf = malloc(p->eeprom_size);
	p->page_dirty = malloc(p->eeprom_size);
	memset(p->eeprom, 0xFF, p->eeprom_size);	// erased EEPROM
	memset(p->page_buf, 0xFF, p->eeprom_size);
	memset(p->page_dirty, 0, p->eeprom_size);

	avr_register_io_write(avr, p->r_base + AVR_NVM_CTRLA, avr_nvm_ctrla_write, p);
	avr_register_io_read(avr, p->r_base + AVR_NVM_STATUS, avr_nvm_status_read, p);

	// intercept writes across the whole mapped EEPROM region (reads fall
	// through to the data space, which mirrors the committed contents)
	for (uint16_t i = 0; i < p->eeprom_size; i++)
		avr_register_io_write(avr, p->eeprom_base + i, avr_nvm_eeprom_write, p);
}
