/*
	avr_nvmctrl_dd.h

	AVR DD (AVR Dx) Nonvolatile Memory Controller. Unlike the tinyAVR/megaAVR
	NVMCTRL (avr_nvmctrl.h) there is no page buffer: a command written to
	NVMCTRL.CTRLA arms an erase or write mode and the following store to the
	memory array performs it. Behaviour follows DS40002413B section 11.

	Modelled: FLWR / FLPER / FLMPERn through stores to the data-space flash
	mapping, EEWR / EEERWR / EEBER / EEMBERn / EECHER on the memory-mapped
	EEPROM, User Row writes/erase, STATUS.ERROR (INVALIDCMD, WRITEPROTECT,
	CMDCOLLISION), the BOOT / APPCODE / APPDATA sections from FUSE.BOOTSIZE and
	FUSE.CODESIZE with the inter-section write protection of Table 11-1 and the
	CTRLB write-protect bits, and the level EEREADY interrupt.

	Simplifications: a store takes effect immediately (STATUS.xBUSY is only held
	for a nominal number of cycles and the CPU is never halted), a store replaces
	the old contents instead of ANDing them, SPM is not decoded, BOOTRP read
	protection is not enforced, and CHER (UPDI only) is ignored when written by
	the CPU. FLMAP is stored but, as the 14/20-pin parts have at most 32 KB of
	flash, the whole flash is always the mapped section.

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

#ifndef __AVR_NVMCTRL_DD_H__
#define __AVR_NVMCTRL_DD_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "sim_avr.h"

/* Register offsets (DS40002413B 11.4). */
enum {
	NVMDD_CTRLA		= 0x00,
	NVMDD_CTRLB		= 0x01,
	NVMDD_STATUS	= 0x02,
	NVMDD_INTCTRL	= 0x03,
	NVMDD_INTFLAGS	= 0x04,
	NVMDD_DATA		= 0x06,
	NVMDD_ADDR		= 0x08,
};

typedef struct avr_nvmctrl_dd_t {
	avr_io_t	io;

	avr_io_addr_t	base;
	avr_io_addr_t	ee_start;		// data address of EEPROM byte 0
	uint16_t		ee_size;
	avr_io_addr_t	uro_start;		// data address of USERROW byte 0
	uint16_t		uro_size;
	avr_io_addr_t	flash_start;	// data address of mapped flash byte 0
	uint32_t		flash_size;
	uint16_t		flash_page;
	uint8_t			fuse_codesize;	// indices into avr->fuse[]
	uint8_t			fuse_bootsize;
	avr_int_vector_t	eeready;
} avr_nvmctrl_dd_t;

/*
 * 'ee_*' / 'uro_*' describe the memory-mapped EEPROM and User Row, 'flash_*'
 * the data-space flash mapping (avr->arch.flashmap_start), and the two fuse
 * indices locate FUSE.CODESIZE / FUSE.BOOTSIZE. 'vec_eeready' is NVMCTRL_EE.
 */
void
avr_nvmctrl_dd_init(
		avr_t * avr,
		avr_nvmctrl_dd_t * p,
		avr_io_addr_t base,
		avr_io_addr_t ee_start, uint16_t ee_size,
		avr_io_addr_t uro_start, uint16_t uro_size,
		avr_io_addr_t flash_start, uint32_t flash_size, uint16_t flash_page,
		uint8_t fuse_codesize_index, uint8_t fuse_bootsize_index,
		uint8_t vec_eeready);

#ifdef __cplusplus
};
#endif

#endif /* __AVR_NVMCTRL_DD_H__ */
