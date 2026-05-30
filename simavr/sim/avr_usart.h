/*
	avr_usart.h

	Modern AVR USART (the AVRxt register-block USART, e.g. USART0 on tinyAVR
	1-series at 0x0800). This models the common asynchronous path: polled or
	interrupt-driven TX (bytes are emitted on the OUTPUT IRQ) and RX injected via
	the INPUT IRQ. Baud timing is not modelled (TX is treated as immediate);
	synchronous / SPI-master / one-wire / IRCOM modes are not modelled.

	The INPUT/OUTPUT IRQ convention matches avr_uart so existing serial tooling
	works.

	Copyright 2024 simavr modern-AVR fork (garyPenhook)

	This file is part of simavr. GNU GPL v3 or later; see COPYING.
 */

#ifndef __AVR_USART_H__
#define __AVR_USART_H__

#include "sim_avr.h"
#include "sim_interrupts.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
	AVR_USART_IRQ_INPUT = 0,	// raise with a byte to feed the receiver
	AVR_USART_IRQ_OUTPUT,		// raised with each transmitted byte
	AVR_USART_IRQ_COUNT
};

// register offsets from the USART base
enum {
	AVR_USART_RXDATAL = 0x00, AVR_USART_RXDATAH, AVR_USART_TXDATAL,
	AVR_USART_TXDATAH, AVR_USART_STATUS, AVR_USART_CTRLA, AVR_USART_CTRLB,
	AVR_USART_CTRLC, AVR_USART_BAUDL, AVR_USART_BAUDH,
};

// STATUS / CTRLA flags (shared bit positions)
#define AVR_USART_DREIF	(1 << 5)
#define AVR_USART_TXCIF	(1 << 6)
#define AVR_USART_RXCIF	(1 << 7)
#define AVR_USART_DREIE	(1 << 5)
#define AVR_USART_TXCIE	(1 << 6)
#define AVR_USART_RXCIE	(1 << 7)
// CTRLB
#define AVR_USART_TXEN	(1 << 6)
#define AVR_USART_RXEN	(1 << 7)

typedef struct avr_usart_t {
	avr_io_t			io;
	char				name;		// '0'
	avr_io_addr_t		r_base;
	avr_int_vector_t	rxc, dre, txc;	// USARTn_RXC/DRE/TXC
} avr_usart_t;

void avr_usart_init(avr_t * avr, avr_usart_t * p);

#define AVR_IOCTL_USART_GETIRQ(_name) AVR_IOCTL_DEF('u','s','r',(_name))

#ifdef __cplusplus
};
#endif

#endif /* __AVR_USART_H__ */
