/*
	avr_usart_modern.c

	"Modern" AVR (AVRxt) USART. See avr_usart_modern.h.

	Model: standard asynchronous TX/RX. Transmitting a byte (write TXDATAL)
	immediately emits it on UART_IRQ_OUTPUT and arms a cycle timer for the frame
	duration after which TXCIF is raised; the data register is treated as always
	empty (DREIF stays set). Receiving (UART_IRQ_INPUT) queues the byte in a
	fifo and raises RXCIF; reading RXDATAL pops it. Frame duration is derived
	from BAUD so cycle-accurate-ish timing and TXC ordering hold; 9-bit, parity,
	sync and one-wire modes are not modelled.

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

#include <stdio.h>
#include <string.h>
#include "avr_usart_modern.h"
#include "sim_cycle_timers.h"

DEFINE_FIFO(uint8_t, usart_rx_fifo);

/* STATUS */
#define RXCIF_bm	0x80
#define TXCIF_bm	0x40
#define DREIF_bm	0x20

/* CTRLA (interrupt enables, same bit positions as STATUS flags) */
#define RXCIE_bm	0x80
#define TXCIE_bm	0x40
#define DREIE_bm	0x20

/* CTRLB */
#define RXEN_bm		0x80
#define TXEN_bm		0x40

static uint32_t usart_frame_cycles(avr_usart_modern_t *p)
{
	avr_t *avr = p->io.avr;
	uint32_t baud = avr->data[p->r_baud] | (avr->data[p->r_baud + 1] << 8);
	/* Async normal: CLK_PER cycles/bit = BAUD/4; a frame is ~10 bits. */
	uint32_t frame = (baud * 10u) / 4u;
	return frame ? frame : 80;
}

/* ---- transmit ---- */

static avr_cycle_count_t
usart_tx_done(struct avr_t *avr, avr_cycle_count_t when, void *param)
{
	avr_usart_modern_t *p = (avr_usart_modern_t *)param;
	avr_core_watch_write(avr, p->r_status, avr->data[p->r_status] | TXCIF_bm);
	if (avr->data[p->r_ctrla] & TXCIE_bm)
		avr_raise_interrupt(avr, &p->txc);
	return 0;
}

static void
avr_usart_modern_txdata_write(struct avr_t *avr, avr_io_addr_t addr,
							  uint8_t v, void *param)
{
	avr_usart_modern_t *p = (avr_usart_modern_t *)param;
	avr_core_watch_write(avr, addr, v);

	if (!(avr->data[p->r_ctrlb] & TXEN_bm))
		return;

	/* Emit on the wire immediately. */
	avr_raise_irq(p->io.irq + UART_IRQ_OUTPUT, v);

	/* New transmission in flight: TXC will assert after the frame; the data
	 * register is free again right away (single-byte instant model). */
	avr_core_watch_write(avr, p->r_status,
			(avr->data[p->r_status] & ~TXCIF_bm) | DREIF_bm);
	avr_cycle_timer_register(avr, usart_frame_cycles(p), usart_tx_done, p);

	/* Data register is empty -> keep feeding a DRE-driven transmitter. */
	if (avr->data[p->r_ctrla] & DREIE_bm)
		avr_raise_interrupt(avr, &p->dre);
}

/* ---- receive ---- */

static void
avr_usart_modern_irq_input(struct avr_irq_t *irq, uint32_t value, void *param)
{
	avr_usart_modern_t *p = (avr_usart_modern_t *)param;
	avr_t *avr = p->io.avr;

	if (!(avr->data[p->r_ctrlb] & RXEN_bm))
		return;
	if (usart_rx_fifo_isfull(&p->rx))
		return;
	usart_rx_fifo_write(&p->rx, value & 0xff);

	avr_core_watch_write(avr, p->r_status, avr->data[p->r_status] | RXCIF_bm);
	if (avr->data[p->r_ctrla] & RXCIE_bm)
		avr_raise_interrupt(avr, &p->rxc);
}

static uint8_t
avr_usart_modern_rxdatal_read(struct avr_t *avr, avr_io_addr_t addr, void *param)
{
	avr_usart_modern_t *p = (avr_usart_modern_t *)param;
	uint8_t v = 0;

	if (!usart_rx_fifo_isempty(&p->rx))
		v = usart_rx_fifo_read(&p->rx);

	if (usart_rx_fifo_isempty(&p->rx)) {
		avr_core_watch_write(avr, p->r_status,
							 avr->data[p->r_status] & ~RXCIF_bm);
		avr_clear_interrupt(avr, &p->rxc);
	}
	avr->data[p->r_rxdatal] = v;
	return v;
}

static uint8_t
avr_usart_modern_rxdatah_read(struct avr_t *avr, avr_io_addr_t addr, void *param)
{
	avr_usart_modern_t *p = (avr_usart_modern_t *)param;
	/* RXDATAH bit7 mirrors RXCIF; data bits / error flags are 0 in this model. */
	return (avr->data[p->r_status] & RXCIF_bm);
}

/* ---- control / status ---- */

static void
avr_usart_modern_status_write(struct avr_t *avr, avr_io_addr_t addr,
							  uint8_t v, void *param)
{
	avr_usart_modern_t *p = (avr_usart_modern_t *)param;
	/* TXCIF is the only writable (W1C) flag; RXCIF/DREIF are read-only. */
	uint8_t cur = avr->data[p->r_status];
	if (v & TXCIF_bm) {
		cur &= ~TXCIF_bm;
		avr_clear_interrupt(avr, &p->txc);
	}
	avr_core_watch_write(avr, p->r_status, cur);
}

static void
avr_usart_modern_ctrla_write(struct avr_t *avr, avr_io_addr_t addr,
							 uint8_t v, void *param)
{
	avr_usart_modern_t *p = (avr_usart_modern_t *)param;
	avr_core_watch_write(avr, p->r_ctrla, v);
	uint8_t st = avr->data[p->r_status];

	/* Newly-enabled, already-pending interrupts fire immediately. */
	if ((v & DREIE_bm) && (st & DREIF_bm))
		avr_raise_interrupt(avr, &p->dre);
	if ((v & RXCIE_bm) && (st & RXCIF_bm))
		avr_raise_interrupt(avr, &p->rxc);
	if ((v & TXCIE_bm) && (st & TXCIF_bm))
		avr_raise_interrupt(avr, &p->txc);
}

static void
avr_usart_modern_reset(avr_io_t *io)
{
	avr_usart_modern_t *p = (avr_usart_modern_t *)io;
	avr_t *avr = p->io.avr;
	usart_rx_fifo_reset(&p->rx);
	/* Data register starts empty. */
	avr->data[p->r_status] = DREIF_bm;
	avr_irq_register_notify(p->io.irq + UART_IRQ_INPUT,
							avr_usart_modern_irq_input, p);
}

static const char *irq_names[UART_IRQ_COUNT] = {
	[UART_IRQ_INPUT] = "8<usart_in",
	[UART_IRQ_OUTPUT] = "8>usart_out",
	[UART_IRQ_OUT_XON] = "<usart_xon",
	[UART_IRQ_OUT_XOFF] = "<usart_xoff",
};

static avr_io_t _io = {
	.kind = "usart_modern",
	.reset = avr_usart_modern_reset,
	.irq_names = irq_names,
};

void
avr_usart_modern_init(
		avr_t * avr,
		avr_usart_modern_t * p,
		avr_io_addr_t base,
		uint8_t vec_rxc,
		uint8_t vec_dre,
		uint8_t vec_txc,
		char name)
{
	memset(p, 0, sizeof(*p));
	p->io = _io;
	p->name = name;
	p->base = base;
	p->r_rxdatal = base + USARTR_RXDATAL;
	p->r_rxdatah = base + USARTR_RXDATAH;
	p->r_txdatal = base + USARTR_TXDATAL;
	p->r_status = base + USARTR_STATUS;
	p->r_ctrla = base + USARTR_CTRLA;
	p->r_ctrlb = base + USARTR_CTRLB;
	p->r_baud = base + USARTR_BAUDL;

	/* RXC: flag in STATUS.RXCIF, enabled by CTRLA.RXCIE (cleared by reading
	 * RXDATA, handled above -> sticky so the engine never auto-clears it). */
	p->rxc.vector = vec_rxc;
	p->rxc.enable.reg = p->r_ctrla;  p->rxc.enable.bit = 7; p->rxc.enable.mask = 1;
	p->rxc.raised.reg = p->r_status; p->rxc.raised.bit = 7; p->rxc.raised.mask = 1;
	p->rxc.raise_sticky = 1;

	p->dre.vector = vec_dre;
	p->dre.enable.reg = p->r_ctrla;  p->dre.enable.bit = 5; p->dre.enable.mask = 1;
	p->dre.raised.reg = p->r_status; p->dre.raised.bit = 5; p->dre.raised.mask = 1;
	p->dre.raise_sticky = 1;

	p->txc.vector = vec_txc;
	p->txc.enable.reg = p->r_ctrla;  p->txc.enable.bit = 6; p->txc.enable.mask = 1;
	p->txc.raised.reg = p->r_status; p->txc.raised.bit = 6; p->txc.raised.mask = 1;
	p->txc.raise_sticky = 1;

	avr_register_io(avr, &p->io);
	avr_register_vector(avr, &p->rxc);
	avr_register_vector(avr, &p->dre);
	avr_register_vector(avr, &p->txc);

	avr_io_setirqs(&p->io, AVR_IOCTL_UART_GETIRQ(p->name), UART_IRQ_COUNT, NULL);

	avr_register_io_write(avr, p->r_txdatal, avr_usart_modern_txdata_write, p);
	avr_register_io_read(avr, p->r_rxdatal, avr_usart_modern_rxdatal_read, p);
	avr_register_io_read(avr, p->r_rxdatah, avr_usart_modern_rxdatah_read, p);
	avr_register_io_write(avr, p->r_status, avr_usart_modern_status_write, p);
	avr_register_io_write(avr, p->r_ctrla, avr_usart_modern_ctrla_write, p);
}
