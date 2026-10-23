/*
	avr_syscfg_dd.h

	AVR Dx System Configuration: SYSCFG (REVID, OCDM registers), the signature
	row (SIGROW: device ID, temperature-sensor calibration words, serial number),
	the read-only FUSE window and the LOCK key. Layouts follow DS40002413B 8.6,
	8.8, 8.9 and 9.3 (and the device header). Unlike the tinyAVR/megaAVR
	avr_syscfg.h the signature row has a 16-byte serial number at 0x10, 16-bit
	TEMPSENSEn words at 0x04/0x06 and no oscillator error bytes.

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

#ifndef __AVR_SYSCFG_DD_H__
#define __AVR_SYSCFG_DD_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "sim_avr.h"

/* Offsets within SIGROW (DS40002413B 8.6.1). */
enum {
	SIGROWDD_DEVICEID0 = 0x00,
	SIGROWDD_TEMPSENSE0 = 0x04,		// 16-bit, gain/slope (unsigned)
	SIGROWDD_TEMPSENSE1 = 0x06,		// 16-bit, offset (signed)
	SIGROWDD_SERNUM0 = 0x10,		// SERNUM0..15
	SIGROWDD_SERNUM_LEN = 16,
};

/* Number of FUSE bytes mapped at the fuse base (WDTCFG .. BOOTSIZE). */
#define AVR_FUSE_DD_COUNT	9

typedef struct avr_syscfg_dd_t {
	avr_io_t	io;
	avr_io_addr_t	syscfg_base, sigrow_base, fuse_base, lock_base;
	uint8_t		revid;
} avr_syscfg_dd_t;

/*
 * The fuses are given their factory-programmed defaults (DS40002413B 8.8.2) here
 * so a firmware image without a .fuse section behaves like a fresh part; an ELF
 * .fuse section loaded later overrides them.
 */
void
avr_syscfg_dd_init(
		avr_t * avr,
		avr_syscfg_dd_t * p,
		avr_io_addr_t syscfg_base,
		avr_io_addr_t sigrow_base,
		avr_io_addr_t fuse_base,
		avr_io_addr_t lock_base,
		uint8_t revid);

#ifdef __cplusplus
};
#endif

#endif /* __AVR_SYSCFG_DD_H__ */
