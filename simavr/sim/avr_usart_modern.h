/*
	avr_usart_modern.h

	"Modern" AVR (AVRxt) USART, as found on the tinyAVR 1-series (ATtiny3217),
	megaAVR-0 and AVR Dx families.

	It reuses the classic UART wire IRQ convention (UART_IRQ_INPUT / _OUTPUT and
	AVR_IOCTL_UART_GETIRQ from avr_uart.h) so the existing simavr UART endpoints
	(uart_pty, uart_udp, the simduino bridge, …) connect to it unchanged; only
	the register set is new (CTRLA/B/C, STATUS, RXDATAL/H, TXDATAL/H, BAUD).

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

#ifndef __AVR_USART_MODERN_H__
#define __AVR_USART_MODERN_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "sim_avr.h"
#include "avr_uart.h"		// UART_IRQ_*, AVR_IOCTL_UART_GETIRQ, fifo macros

DECLARE_FIFO(uint8_t, usart_rx_fifo, 256);

/* Register offsets within a USART block (device header USART_t). */
enum {
	USARTR_RXDATAL = 0x00,
	USARTR_RXDATAH = 0x01,
	USARTR_TXDATAL = 0x02,
	USARTR_TXDATAH = 0x03,
	USARTR_STATUS = 0x04,
	USARTR_CTRLA = 0x05,
	USARTR_CTRLB = 0x06,
	USARTR_CTRLC = 0x07,
	USARTR_BAUDL = 0x08,
	USARTR_BAUDH = 0x09,
};

/*
 * Extra (modern-only) IRQ after the classic UART_IRQ_* set: what the transmitter
 * does to its TXD pin, value bit 0 = line level, bit 1 = the transmitter
 * overrides the pin (CTRLB.TXEN set, or a frame still being shifted out). The
 * level follows the real frame: idle high, Start low, data LSB first, optional
 * parity, Stop high. PORTMUX routes it to PB2 / PA1 etc.
 */
#define USART_MODERN_IRQ_TXD_PIN	UART_IRQ_COUNT
#define USART_MODERN_IRQ_COUNT		(UART_IRQ_COUNT + 1)

#define USART_TXQ_SIZE	64	/* bytes waiting to be shifted onto the TXD pin */

typedef struct avr_usart_modern_t {
	avr_io_t	io;
	char		name;

	avr_io_addr_t	base;
	avr_io_addr_t	r_rxdatal, r_rxdatah, r_txdatal, r_status;
	avr_io_addr_t	r_ctrla, r_ctrlb, r_ctrlc, r_baud;

	avr_int_vector_t	rxc;	// USARTn_RXC (receive complete)
	avr_int_vector_t	dre;	// USARTn_DRE (data register empty)
	avr_int_vector_t	txc;	// USARTn_TXC (transmit complete)

	usart_rx_fifo_t	rx;		// bytes received from the outside, awaiting read
	uint8_t			tx_data;	// last byte written, completes on tx_done
	uint8_t			tx_loopback;	// deliver tx_data into RX path on tx_done

	/* TXD pin waveform: queued bytes, the frame being shifted (bit 0 first),
	 * and the last pin state published. */
	uint8_t			txq[USART_TXQ_SIZE];
	uint8_t			txq_head, txq_count;
	uint16_t		frame_bits;
	uint8_t			frame_len, frame_pos;	// frame_pos == frame_len: not shifting
	uint8_t			pin_state;
} avr_usart_modern_t;

/*
 * Initialise a USART block at data address 'base'. The RXC/DRE/TXC interrupt
 * vector numbers come from the device header; 'name' tags the wire IRQs.
 */
void
avr_usart_modern_init(
		avr_t * avr,
		avr_usart_modern_t * p,
		avr_io_addr_t base,
		uint8_t vec_rxc,
		uint8_t vec_dre,
		uint8_t vec_txc,
		char name);

#ifdef __cplusplus
};
#endif

#endif /* __AVR_USART_MODERN_H__ */
