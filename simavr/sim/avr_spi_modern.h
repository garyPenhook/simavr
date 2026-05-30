/*
	avr_spi_modern.h

	"Modern" AVR (AVRxt) SPI (the register-block SPI, e.g. SPI0 at 0x0820 on the
	tinyAVR 1-series, also megaAVR-0 and AVR Dx families).

	Models the normal (non-buffered) mode in host (master) and client (slave)
	roles, driving the *same* wire IRQ convention as the classic avr_spi.c
	(SPI_IRQ_INPUT / SPI_IRQ_OUTPUT, AVR_IOCTL_SPI_GETIRQ) so existing simavr SPI
	endpoints connect unchanged — only the register glue is new.

	Not modelled: buffered mode (CTRLB.BUFEN — the RXCIF/TXCIF/DREIF/SSIF/BUFOVF
	flag set and their separate enables); the SS client-select trigger; exact
	SPI modes (CPOL/CPHA) and bit order beyond storing the configuration.

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

#ifndef __AVR_SPI_MODERN_H__
#define __AVR_SPI_MODERN_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "sim_avr.h"
#include "avr_spi.h"	/* SPI_IRQ_INPUT/OUTPUT, AVR_IOCTL_SPI_GETIRQ */

/* Register offsets within a modern SPI block (device header SPI_t). */
enum {
	SPIMR_CTRLA = 0x00,
	SPIMR_CTRLB = 0x01,
	SPIMR_INTCTRL = 0x02,
	SPIMR_INTFLAGS = 0x03,
	SPIMR_DATA = 0x04,
};

typedef struct avr_spi_modern_t {
	avr_io_t	io;
	char		name;		/* '0', … */

	avr_io_addr_t	base;
	avr_io_addr_t	r_ctrla, r_ctrlb, r_intctrl, r_intflags, r_data;

	avr_int_vector_t	vect;	/* SPIn_INT (normal-mode IF, enabled by IE) */

	uint8_t		busy;		/* a transfer is in flight (host) */
} avr_spi_modern_t;

/*
 * Initialise a modern SPI block at data address 'base'. 'vector' is the
 * SPIn_INT vector number; 'name' is a tag for debug and the IRQ ioctl.
 */
void
avr_spi_modern_init(
		avr_t * avr,
		avr_spi_modern_t * p,
		avr_io_addr_t base,
		uint8_t vector,
		char name);

#ifdef __cplusplus
};
#endif

#endif /* __AVR_SPI_MODERN_H__ */
