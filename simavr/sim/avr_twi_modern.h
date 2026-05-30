/*
	avr_twi_modern.h

	"Modern" AVR (AVRxt) Two-Wire Interface (TWI0) peripheral, as found on the
	tinyAVR 1-series (e.g. ATtiny3217), megaAVR-0 and AVR Dx families.

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

#ifndef __AVR_TWI_MODERN_H__
#define __AVR_TWI_MODERN_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "sim_avr.h"
#include "avr_twi.h"		// reuse the wire IRQ protocol (TWI_IRQ_*, TWI_COND_*)

/*
 * The modern TWI is a single register block of 16 bytes. Unlike the classic
 * TWI (a handful of registers scattered by the core descriptor), the modern
 * peripheral is described by a single base address; every register is at a
 * fixed offset from it. The layout matches the device header TWI_t struct:
 *
 *   +0x00 CTRLA     +0x07 MADDR
 *   +0x02 DBGCTRL   +0x08 MDATA
 *   +0x03 MCTRLA    +0x09 SCTRLA
 *   +0x04 MCTRLB    +0x0A SCTRLB
 *   +0x05 MSTATUS   +0x0B SSTATUS
 *   +0x06 MBAUD     +0x0C SADDR
 *                   +0x0D SDATA
 *                   +0x0E SADDRMASK
 */
enum {
	TWIM_CTRLA = 0x00,
	TWIM_DBGCTRL = 0x02,
	TWIM_MCTRLA = 0x03,
	TWIM_MCTRLB = 0x04,
	TWIM_MSTATUS = 0x05,
	TWIM_MBAUD = 0x06,
	TWIM_MADDR = 0x07,
	TWIM_MDATA = 0x08,
	TWIM_SCTRLA = 0x09,
	TWIM_SCTRLB = 0x0a,
	TWIM_SSTATUS = 0x0b,
	TWIM_SADDR = 0x0c,
	TWIM_SDATA = 0x0d,
	TWIM_SADDRMASK = 0x0e,
	TWIM_REG_SIZE = 0x10,
};

typedef struct avr_twi_modern_t {
	avr_io_t	io;
	char		name;

	avr_io_addr_t	base;	// data address of CTRLA; all regs are base + offset

	/* Resolved register addresses (base + offset), filled in by init */
	avr_io_addr_t	r_ctrla, r_dbgctrl;
	avr_io_addr_t	r_mctrla, r_mctrlb, r_mstatus, r_mbaud, r_maddr, r_mdata;
	avr_io_addr_t	r_sctrla, r_sctrlb, r_sstatus, r_saddr, r_sdata, r_saddrmask;

	avr_int_vector_t	mvector;	// TWIM (host) interrupt vector
	avr_int_vector_t	svector;	// TWIS (client) interrupt vector

	/*
	 * Transient master state. The wire protocol is delivered synchronously
	 * (raising the OUTPUT IRQ runs the connected slave's hook, whose reply
	 * lands on our INPUT IRQ before control returns), so these capture the
	 * reply of the in-flight byte.
	 */
	uint8_t		m_dir;		// current master transfer: 1 = read, 0 = write
	uint8_t		m_busy;		// 1 while a master wire op is in flight (re-entrancy guard)
	uint8_t		m_ack;		// 1 if the last addressed/written byte was ACKed
	uint8_t		m_rx_valid;	// 1 if m_rx_data holds a freshly received byte
	uint8_t		m_rx_data;	// data byte received from the slave

	/* Slave state */
	uint8_t		s_selected;	// non-zero when addressed as a slave (holds addr|R/W)
} avr_twi_modern_t;

/*
 * Initialise the modern TWI register block at data address 'base'.
 * 'mvector'/'svector' are the host (TWIM) and client (TWIS) interrupt vector
 * numbers from the device's CPUINT vector table (e.g. 25 and 24 on ATtiny3217).
 * 'name' is a tag used for the wire IRQ ioctl, like the classic TWI.
 */
void
avr_twi_modern_init(
		avr_t * avr,
		avr_twi_modern_t * p,
		avr_io_addr_t base,
		uint8_t mvector,
		uint8_t svector,
		char name);

#ifdef __cplusplus
};
#endif

#endif /* __AVR_TWI_MODERN_H__ */
