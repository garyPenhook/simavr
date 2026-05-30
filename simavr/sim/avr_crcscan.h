/*
	avr_crcscan.h

	"Modern" AVR (AVRxt) CRC memory scan (CRCSCAN at 0x0120 on the tinyAVR
	1-series, also megaAVR-0 and AVR Dx families).

	CRCSCAN computes a CRC over a flash section and compares it with a checksum
	programmed into the last two bytes of that section; on a match STATUS.OK is
	set, on a mismatch OK is cleared and — if CTRLA.NMIEN is set — the NMI
	(vector 1) is raised.

	This computes the CRC for real. Enabling the scan (CTRLA.ENABLE) runs a
	CRC-16-CCITT (polynomial 0x1021, initial value 0xFFFF, MSB-first, byte-wise in
	ascending address order) over the selected section (CTRLB.SRC: full flash /
	boot+application / boot), excluding the trailing two checksum bytes, and
	compares it against the stored big-endian checksum at the section end (see
	datasheet Table 27-1). The completion is instant (STATUS.BUSY never observed
	set). The CTRLA.NMIEN lock (once the NMI is armed the peripheral cannot be
	disabled until reset) and the CTRLA.RESET strobe are modelled.

	Not modelled: the boot-time fuse-driven scan (FUSE.SYSCFG0.CRCSRC) that hangs
	the CPU on failure before code starts — only the software-enabled scan and its
	NMI path are modelled. The CRC initial value follows CRC-16/CCITT-FALSE; a
	toolchain using a different convention can be matched by adjusting it.

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

#ifndef __AVR_CRCSCAN_H__
#define __AVR_CRCSCAN_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "sim_avr.h"

/* Register offsets within a CRCSCAN block (device header CRCSCAN_t). */
enum {
	CRCSCANR_CTRLA = 0x00,
	CRCSCANR_CTRLB = 0x01,
	CRCSCANR_STATUS = 0x02,
};

typedef struct avr_crcscan_t {
	avr_io_t	io;
	char		name;

	avr_io_addr_t	base;
	avr_io_addr_t	r_ctrla, r_ctrlb, r_status;

	uint32_t	flash_size;	/* bytes of flash to scan over (full-flash end) */
	uint8_t		append_idx;	/* FUSE.APPEND index (boot+app section end) */
	uint8_t		bootend_idx;	/* FUSE.BOOTEND index (boot section end) */

	avr_int_vector_t	nmi;	/* CRC-failure NMI (vector 1) */

	uint8_t		locked;		/* NMIEN set => CTRLA read-only until reset */
} avr_crcscan_t;

/*
 * CRC-16-CCITT (poly 0x1021, init 0xFFFF, MSB-first) over 'len' bytes. Exposed
 * so a test/board can compute a matching section checksum.
 */
uint16_t
avr_crcscan_crc16(const uint8_t * data, uint32_t len);

/*
 * Initialise a CRCSCAN block at data address 'base'. 'flash_size' is the flash
 * size (full-flash section end); 'append_fuse_index'/'bootend_fuse_index' locate
 * the APPEND/BOOTEND fuses that bound the application/boot sections (0xff to
 * skip). 'nmi_vector' is the CRC-failure NMI vector. 'name' is a debug tag.
 */
void
avr_crcscan_init(
		avr_t * avr,
		avr_crcscan_t * p,
		avr_io_addr_t base,
		uint32_t flash_size,
		uint8_t append_fuse_index,
		uint8_t bootend_fuse_index,
		uint8_t nmi_vector,
		char name);

#ifdef __cplusplus
};
#endif

#endif /* __AVR_CRCSCAN_H__ */
