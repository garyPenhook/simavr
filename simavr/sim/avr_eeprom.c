/*
	avr_eeprom.c

	IO module that simulates the AVR EEProm

	Copyright 2008, 2009 Michel Pollet <buserror@gmail.com>

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
#include <stdlib.h>
#include <string.h>
#include "avr_eeprom.h"

static avr_cycle_count_t
avr_eempe_clear(
		struct avr_t * avr,
		avr_cycle_count_t when,
		void * param)
{
	avr_eeprom_t * p = (avr_eeprom_t *)param;
	avr_regbit_clear(p->io.avr, p->eempe);
	return 0;
}

static avr_cycle_count_t
		avr_eei_raise(struct avr_t * avr,
		avr_cycle_count_t when,
		void * param)
{
	avr_eeprom_t * p = (avr_eeprom_t *)param;
	avr_raise_interrupt(p->io.avr, &p->ready);
	return 0;
}

static void
avr_eeprom_write(
		avr_t * avr,
		avr_io_addr_t addr,
		uint8_t v,
		void * param)
{
	avr_eeprom_t * p = (avr_eeprom_t *)param;
	uint8_t eempe = avr_regbit_get(avr, p->eempe);

	avr_core_watch_write(avr, addr, v);

	if (!eempe && avr_regbit_get(avr, p->eempe)) {
		avr_cycle_timer_register(avr, 4, avr_eempe_clear, p);
	}

	uint16_t ee_addr;
	if (p->r_eearh)
		ee_addr = avr->data[p->r_eearl] | (avr->data[p->r_eearh] << 8);
	else
		ee_addr = avr->data[p->r_eearl];
	if (((eempe && avr_regbit_get(avr, p->eepe)) || avr_regbit_get(avr, p->eere)) &&
			ee_addr >= p->size) {
		AVR_LOG(avr, LOG_ERROR, "EEPROM: *** %s address out of bounds: %04x > %04x,"
								" wrapping to %04x (PC=%04x)\n",
				eempe ? "Write" : "Read",
				ee_addr, p->size-1, ee_addr & (p->size-1),
				avr->pc);
		ee_addr = ee_addr & (p->size-1);
	}
	if (eempe && avr_regbit_get(avr, p->eepe)) {	// write operation
		//	printf("eeprom write %04x <- %02x\n", addr, avr->data[p->r_eedr]);
		p->eeprom[ee_addr] = avr->data[p->r_eedr];
		// Automatically clears that bit (?)
		avr_regbit_clear(avr, p->eempe);

		avr_cycle_timer_register_usec(avr, 3400, avr_eei_raise, p); // 3.4ms here
	}
	if (avr_regbit_get(avr, p->eere)) {	// read operation
		avr->data[p->r_eedr] = p->eeprom[ee_addr];
		//	printf("eeprom read %04x : %02x\n", addr, p->eeprom[addr]);
	}

	// autocleared
	avr_regbit_clear(avr, p->eepe);
	avr_regbit_clear(avr, p->eere);
}

/*
 * Datasheet timing (DS40002307A 5.3, 5.5.3), selected with .timed.
 */

#define EE_RDY_RETRY_CYCLES	64

static avr_cycle_count_t avr_eeprom_ready_retry(struct avr_t *, avr_cycle_count_t, void *);

static avr_cycle_count_t
avr_eeprom_done(
		struct avr_t * avr,
		avr_cycle_count_t when,
		void * param)
{
	avr_eeprom_t * p = (avr_eeprom_t *)param;

	switch (p->w_mode) {
		case 1:		// erase only
			p->eeprom[p->w_addr] = 0xff;
			break;
		case 2:		// write only: programming can only clear bits
			p->eeprom[p->w_addr] &= p->w_data;
			break;
		default:	// erase and write
			p->eeprom[p->w_addr] = p->w_data;
			break;
	}
	p->busy = 0;
	avr_regbit_clear(avr, p->eepe);
	// the ready interrupt is requested for as long as EERIE is set and the EEPROM is idle
	if (!avr_raise_interrupt(avr, &p->ready) && avr_regbit_get(avr, p->ready.enable))
		avr_cycle_timer_register(avr, EE_RDY_RETRY_CYCLES, avr_eeprom_ready_retry, p);
	return 0;
}

/* the request was dropped (the sleep mode has no clock for it): ask again until it is accepted */
static avr_cycle_count_t
avr_eeprom_ready_retry(
		struct avr_t * avr,
		avr_cycle_count_t when,
		void * param)
{
	avr_eeprom_t * p = (avr_eeprom_t *)param;

	if (p->busy || !avr_regbit_get(avr, p->ready.enable))
		return 0;
	return avr_raise_interrupt(avr, &p->ready) ? 0 : when + EE_RDY_RETRY_CYCLES;
}

static void
avr_eeprom_write_timed(
		avr_t * avr,
		avr_io_addr_t addr,
		uint8_t v,
		void * param)
{
	avr_eeprom_t * p = (avr_eeprom_t *)param;
	uint8_t eempe = avr_regbit_get(avr, p->eempe);
	uint8_t old_eerie = avr_regbit_get(avr, p->ready.enable);
	uint8_t old = avr->data[addr];

	avr_core_watch_write(avr, addr, v);

	if (p->busy) {
		// EEPMn can not change, and EEPE / EEMPE can not be set again, while programming
		for (int i = 0; i < 2; i++)
			avr_regbit_setto(avr, p->eepm[i], (old >> p->eepm[i].bit) & 1);
		avr_regbit_set(avr, p->eepe);
		avr_regbit_setto(avr, p->eempe, eempe);
	} else if (!eempe && avr_regbit_get(avr, p->eempe)) {
		avr_cycle_timer_register(avr, 4, avr_eempe_clear, p);
	}

	uint16_t ee_addr = p->r_eearh ?
			avr->data[p->r_eearl] | (avr->data[p->r_eearh] << 8) : avr->data[p->r_eearl];
	if (ee_addr >= p->size)
		ee_addr &= p->size - 1;

	if (!p->busy && eempe && ((v >> p->eepe.bit) & 1)) {		// write operation
		uint8_t mode = avr_regbit_get(avr, p->eepm[0]) | (avr_regbit_get(avr, p->eepm[1]) << 1);

		avr_regbit_clear(avr, p->eempe);
		avr_cycle_timer_cancel(avr, avr_eempe_clear, p);
		if (mode != 3) {		// 0b11 is reserved: nothing happens
			p->busy = 1;
			p->w_mode = mode;
			p->w_addr = ee_addr;
			p->w_data = avr->data[p->r_eedr];
			avr_regbit_set(avr, p->eepe);
			avr->cycle += 2;	// the CPU is halted for two cycles
			avr_cycle_timer_register_usec(avr, mode ? 1800 : 3400, avr_eeprom_done, p);
		}
	}
	if (!p->busy && ((v >> p->eere.bit) & 1)) {			// read operation
		avr->data[p->r_eedr] = p->eeprom[ee_addr];
		avr->cycle += 4;		// the CPU is halted for four cycles
	}

	// EERE is a strobe, EEPE reads back as 1 only while programming
	avr_regbit_clear(avr, p->eere);
	if (!p->busy)
		avr_regbit_clear(avr, p->eepe);

	if (!old_eerie && avr_regbit_get(avr, p->ready.enable) && !p->busy)
		avr_raise_interrupt(avr, &p->ready);
}

/* servicing the ready interrupt does not clear its cause: request it again while EERIE is set and idle */
static void
avr_eeprom_ready_notify(
		struct avr_irq_t * irq,
		uint32_t value,
		void * param)
{
	avr_eeprom_t * p = (avr_eeprom_t *)param;
	avr_t * avr = p->io.avr;

	if (!value && !p->busy && avr_regbit_get(avr, p->ready.enable))
		avr_raise_interrupt(avr, &p->ready);
}

static int
avr_eeprom_ioctl(
		struct avr_io_t * port,
		uint32_t ctl,
		void * io_param)
{
	avr_eeprom_t * p = (avr_eeprom_t *)port;
	int res = -1;

	switch(ctl) {
		case AVR_IOCTL_EEPROM_SET: {
			avr_eeprom_desc_t * desc = (avr_eeprom_desc_t*)io_param;
			if (!desc || !desc->size || !desc->ee || (desc->offset + desc->size) > p->size) {
				AVR_LOG(port->avr, LOG_WARNING, "EEPROM: %s: AVR_IOCTL_EEPROM_SET Invalid argument\n",
						__FUNCTION__);
				return -2;
			}
			memcpy(p->eeprom + desc->offset, desc->ee, desc->size);
			AVR_LOG(port->avr, LOG_TRACE, "EEPROM: %s: AVR_IOCTL_EEPROM_SET Loaded %d at offset %d\n",
					__FUNCTION__, desc->size, desc->offset);
			res = 0;
		}	break;
		case AVR_IOCTL_EEPROM_GET: {
			avr_eeprom_desc_t * desc = (avr_eeprom_desc_t*)io_param;
			if (!desc || (desc->offset + desc->size) > p->size) {
				AVR_LOG(port->avr, LOG_WARNING, "EEPROM: %s: AVR_IOCTL_EEPROM_GET Invalid argument\n",
						__FUNCTION__);
				return -2;
			}
			if (desc->ee)
				memcpy(desc->ee, p->eeprom + desc->offset, desc->size);
			else	// allow to get access to the read data, for gdb support
				desc->ee = p->eeprom + desc->offset;
			res = 0;
		}	break;
	}

	return res;
}

static void
avr_eeprom_dealloc(
		struct avr_io_t * port)
{
	avr_eeprom_t * p = (avr_eeprom_t *)port;
	if (p->eeprom)
		free(p->eeprom);
	p->eeprom = NULL;
}

static void
avr_eeprom_reset(
		struct avr_io_t * port)
{
	avr_eeprom_t * p = (avr_eeprom_t *)port;

	p->busy = 0;
	if (p->timed)
		avr_irq_register_notify(p->ready.irq + AVR_INT_IRQ_PENDING, avr_eeprom_ready_notify, p);
}

static	avr_io_t	_io = {
	.kind = "eeprom",
	.reset = avr_eeprom_reset,
	.ioctl = avr_eeprom_ioctl,
	.dealloc = avr_eeprom_dealloc,
};

void
avr_eeprom_init(
		avr_t * avr,
		avr_eeprom_t * p)
{
	p->io = _io;
//	printf("%s init (%d bytes) EEL/H:%02x/%02x EED=%02x EEC=%02x\n",
//			__FUNCTION__, p->size, p->r_eearl, p->r_eearh, p->r_eedr, p->r_eecr);

	p->eeprom = malloc(p->size);
	memset(p->eeprom, 0xff, p->size);

	avr_register_io(avr, &p->io);
	avr_register_vector(avr, &p->ready);

	if (p->timed)
		avr_register_io_write(avr, p->r_eecr, avr_eeprom_write_timed, p);
	else
		avr_register_io_write(avr, p->r_eecr, avr_eeprom_write, p);
}

