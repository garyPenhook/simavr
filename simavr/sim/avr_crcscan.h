/*
	avr_crcscan.h

	"Modern" AVR (AVRxt) CRC memory scan (CRCSCAN at 0x0120 on the tinyAVR
	1-series, also megaAVR-0 and AVR Dx families).

	On hardware CRCSCAN computes a CRC over a flash section and compares it with a
	checksum programmed into the last bytes of that section; a mismatch either
	resets the device or raises the NMI (vector 1), depending on CTRLA.NMIEN.

	A loaded simulation image has no authoritative external checksum to validate
	against, so this models the scan as completing instantly and reporting OK
	(STATUS.BUSY is never observed set), which lets firmware that enables CRCSCAN
	and waits for STATUS.OK proceed. The CTRLA.NMIEN configuration lock (once the
	NMI is enabled the peripheral cannot be disabled until reset) and the
	CTRLA.RESET strobe are modelled.

	Not modelled: the actual CRC computation and the mismatch-triggered
	reset/NMI (the scan always reports OK on the loaded image).

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
	avr_io_addr_t	r_ctrla, r_status;

	uint8_t		locked;		/* NMIEN set => CTRLA read-only until reset */
} avr_crcscan_t;

/*
 * Initialise a CRCSCAN block at data address 'base'. 'name' is a tag for debug.
 */
void
avr_crcscan_init(
		avr_t * avr,
		avr_crcscan_t * p,
		avr_io_addr_t base,
		char name);

#ifdef __cplusplus
};
#endif

#endif /* __AVR_CRCSCAN_H__ */
