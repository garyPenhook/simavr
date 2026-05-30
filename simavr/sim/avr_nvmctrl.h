/*
	avr_nvmctrl.h

	"Modern" AVR (AVRxt) Non-Volatile Memory Controller (NVMCTRL), as found on
	the tinyAVR 1-series (ATtiny3217), megaAVR-0 and AVR Dx families. It replaces
	the classic EECR/EEDR/EEAR and SPMCSR registers with a command-register
	model over memory-mapped EEPROM and flash.

	This models EEPROM programming: the EEPROM is mapped into the data space;
	writing to it loads a page buffer, and a (CCP-protected) command in
	NVMCTRL.CTRLA commits, erases, or clears it. Flash self-programming is not
	modelled yet.

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

#ifndef __AVR_NVMCTRL_H__
#define __AVR_NVMCTRL_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "sim_avr.h"

/* Register offsets within the NVMCTRL block (device header NVMCTRL_t). */
enum {
	NVMR_CTRLA = 0x00,
	NVMR_CTRLB = 0x01,
	NVMR_STATUS = 0x02,
	NVMR_INTCTRL = 0x03,
	NVMR_INTFLAGS = 0x04,
	NVMR_DATAL = 0x06,
	NVMR_ADDRL = 0x08,
};

#define AVR_NVM_EE_MAX 512	/* max modelled EEPROM size */

typedef struct avr_nvmctrl_t {
	avr_io_t	io;

	avr_io_addr_t	base;
	avr_io_addr_t	r_ctrla, r_status, r_intctrl, r_intflags;

	avr_io_addr_t	ee_start;	// data address of mapped EEPROM byte 0
	uint16_t	ee_size;	// EEPROM size in bytes

	avr_int_vector_t	eeready;	// NVMCTRL_EE (EEPROM ready)

	/* EEPROM page buffer: bytes written to the mapped region accumulate here
	 * until a commit command moves the dirty ones into the committed EEPROM
	 * (which lives directly in avr->data[ee_start..]). */
	uint8_t		buf[AVR_NVM_EE_MAX];
	uint8_t		dirty[AVR_NVM_EE_MAX];
} avr_nvmctrl_t;

/*
 * Initialise NVMCTRL at data address 'base'. 'ee_start'/'ee_size' describe the
 * memory-mapped EEPROM; 'vec_eeready' is the EEPROM-ready interrupt vector.
 */
void
avr_nvmctrl_init(
		avr_t * avr,
		avr_nvmctrl_t * p,
		avr_io_addr_t base,
		avr_io_addr_t ee_start,
		uint16_t ee_size,
		uint8_t vec_eeready);

#ifdef __cplusplus
};
#endif

#endif /* __AVR_NVMCTRL_H__ */
