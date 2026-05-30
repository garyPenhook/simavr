/*
	avr_spi_modern.h

	Modern AVR SPI (the AVRxt register-block SPI, e.g. SPI0 on tinyAVR 1-series
	at 0x0820). Models normal (non-buffered) master mode: writing DATA starts a
	transfer, the byte is emitted on the OUTPUT IRQ (MOSI), the value most
	recently presented on the INPUT IRQ (MISO) is latched as the received byte,
	the interrupt flag (IF) is set and the SPI interrupt raised if enabled.
	Buffer mode, slave mode and clock timing are not modelled.

	Copyright 2024 simavr modern-AVR fork (garyPenhook)

	This file is part of simavr. GNU GPL v3 or later; see COPYING.
 */

#ifndef __AVR_SPI_MODERN_H__
#define __AVR_SPI_MODERN_H__

#include "sim_avr.h"
#include "sim_interrupts.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
	AVR_SPIM_IRQ_INPUT = 0,		// present a byte here to be received (MISO)
	AVR_SPIM_IRQ_OUTPUT,		// raised with each transmitted byte (MOSI)
	AVR_SPIM_IRQ_COUNT
};

// register offsets from the SPI base
enum {
	AVR_SPIM_CTRLA = 0x00, AVR_SPIM_CTRLB = 0x01,
	AVR_SPIM_INTCTRL = 0x02, AVR_SPIM_INTFLAGS = 0x03, AVR_SPIM_DATA = 0x04,
};

#define AVR_SPIM_ENABLE	(1 << 0)	// CTRLA.ENABLE
#define AVR_SPIM_MASTER	(1 << 5)	// CTRLA.MASTER
#define AVR_SPIM_IE		(1 << 0)	// INTCTRL.IE (normal mode)
#define AVR_SPIM_WRCOL	(1 << 6)	// INTFLAGS.WRCOL
#define AVR_SPIM_IF		(1 << 7)	// INTFLAGS.IF (normal mode)

typedef struct avr_spim_t {
	avr_io_t			io;
	char				name;		// '0'
	avr_io_addr_t		r_base;
	avr_int_vector_t	spi;		// SPIn_INT
	uint8_t				miso;		// latched value to receive
} avr_spim_t;

void avr_spim_init(avr_t * avr, avr_spim_t * p);

#define AVR_IOCTL_SPIM_GETIRQ(_name) AVR_IOCTL_DEF('s','p','m',(_name))

#ifdef __cplusplus
};
#endif

#endif /* __AVR_SPI_MODERN_H__ */
