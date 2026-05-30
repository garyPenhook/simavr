/*
	avr_syscfg.h

	"Modern" AVR (AVRxt) device-identity registers: the System Configuration
	block (SYSCFG at 0x0F00 on the tinyAVR 1-series) and the Signature Row
	(SIGROW at 0x1100). Both hold read-only device information.

	SYSCFG.REVID reports the silicon revision (0x00 = A, 0x01 = B, …).
	SYSCFG.EXTBRK enables the OCD external-break feature and is the one writable
	register here (modelled as a store; the debug pin is not simulated).

	SIGROW.DEVICEID[2:0] is the three-byte device signature that firmware reads to
	identify the part (0x1E 0x95 0x22 for the ATtiny3217); it is populated from
	the core's signature[]. The serial-number (SERNUM) and calibration
	(TEMPSENSE, OSCnnERRxV) bytes are device-unique factory data with no canonical
	simulator value and are left as plain (zero) read-only storage.

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

#ifndef __AVR_SYSCFG_H__
#define __AVR_SYSCFG_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "sim_avr.h"

/* Register offsets within the SYSCFG block (device header SYSCFG_t). */
enum {
	SYSCFGR_REVID = 0x01,
	SYSCFGR_EXTBRK = 0x02,
};

/* Register offsets within the SIGROW block (device header SIGROW_t). */
enum {
	SIGROWR_DEVICEID0 = 0x00,
	SIGROWR_DEVICEID1 = 0x01,
	SIGROWR_DEVICEID2 = 0x02,
	SIGROWR_TEMPSENSE0 = 0x20,	/* temp-sensor gain/slope */
	SIGROWR_TEMPSENSE1 = 0x21,	/* temp-sensor offset */
};

/* Representative temperature-sensor calibration loaded into SIGROW. With these,
 * the datasheet transfer function T_K = ((adc - off)*gain + 0x80) >> 8 reduces
 * to adc = off + 2*T_K (gain 128 => an exact integer round-trip). */
#define AVR_SIGROW_TEMPSENSE0_CAL	128	/* gain */
#define AVR_SIGROW_TEMPSENSE1_CAL	50	/* offset */

typedef struct avr_syscfg_t {
	avr_io_t	io;

	avr_io_addr_t	syscfg_base;
	avr_io_addr_t	sigrow_base;
	uint8_t		revid;		/* SYSCFG.REVID value (0 = rev A) */
} avr_syscfg_t;

/*
 * Initialise the SYSCFG block at 'syscfg_base' and the signature row at
 * 'sigrow_base'. 'revid' is the reported silicon revision. SIGROW.DEVICEID is
 * taken from avr->signature[].
 */
void
avr_syscfg_init(
		avr_t * avr,
		avr_syscfg_t * p,
		avr_io_addr_t syscfg_base,
		avr_io_addr_t sigrow_base,
		uint8_t revid);

#ifdef __cplusplus
};
#endif

#endif /* __AVR_SYSCFG_H__ */
