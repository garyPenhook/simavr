/*
	avr_nvmctrl.h

	Modern AVR Non-Volatile Memory controller (NVMCTRL), EEPROM path. The EEPROM
	is memory-mapped (MAPPED_EEPROM): writes to the mapped region load a page
	buffer, and a command written to NVMCTRL.CTRLA (PAGEWRITE / PAGEERASEWRITE /
	EEERASE / page-buffer-clear) commits or erases. EEPROM contents live in a
	persistent buffer so they survive avr_reset(); the committed values are also
	mirrored into the data space for direct mapped reads.

	Flash self-programming (the SPM path through the mapped flash window) is not
	yet modelled.

	Copyright 2024 simavr modern-AVR fork (garyPenhook)

	This file is part of simavr. GNU GPL v3 or later; see COPYING.
 */

#ifndef __AVR_NVMCTRL_H__
#define __AVR_NVMCTRL_H__

#include "sim_avr.h"
#include "sim_interrupts.h"

#ifdef __cplusplus
extern "C" {
#endif

// register offsets from the NVMCTRL base
enum {
	AVR_NVM_CTRLA = 0x00, AVR_NVM_CTRLB = 0x01, AVR_NVM_STATUS = 0x02,
	AVR_NVM_INTCTRL = 0x03, AVR_NVM_INTFLAGS = 0x04,
	AVR_NVM_DATA = 0x06, AVR_NVM_ADDR = 0x08,
};

// CTRLA.CMD values
#define AVR_NVM_CMD_NONE		0x00
#define AVR_NVM_CMD_PAGEWRITE		0x01
#define AVR_NVM_CMD_PAGEERASE		0x02
#define AVR_NVM_CMD_PAGEERASEWRITE	0x03
#define AVR_NVM_CMD_PAGEBUFCLR		0x04
#define AVR_NVM_CMD_CHIPERASE		0x05
#define AVR_NVM_CMD_EEERASE		0x06
#define AVR_NVM_CMD_CMD_gm		0x07
// STATUS / INTFLAGS bits
#define AVR_NVM_FBUSY	(1 << 0)
#define AVR_NVM_EEBUSY	(1 << 1)
#define AVR_NVM_EEREADY	(1 << 0)

typedef struct avr_nvmctrl_t {
	avr_io_t			io;
	avr_io_addr_t		r_base;			// NVMCTRL registers (0x1000)
	avr_io_addr_t		eeprom_base;	// mapped EEPROM (0x1400)
	uint16_t			eeprom_size;	// 256
	uint16_t			page_size;		// 64
	avr_int_vector_t	eeready;		// NVMCTRL_EE

	uint8_t *			eeprom;			// persistent EEPROM contents
	uint8_t *			page_buf;		// staged write data
	uint8_t *			page_dirty;		// which offsets have been staged
} avr_nvmctrl_t;

void avr_nvmctrl_init(avr_t * avr, avr_nvmctrl_t * p);

#ifdef __cplusplus
};
#endif

#endif /* __AVR_NVMCTRL_H__ */
