/*
	avr_spi_modern.c

	"Modern" AVR (AVRxt) register-block SPI, normal (non-buffered) mode. See
	avr_spi_modern.h.

	Wire model (identical to the classic avr_spi.c, synchronous): in host mode,
	writing DATA schedules a transfer of (prescaler * 8) CPU cycles; on
	completion the byte is emitted on SPI_IRQ_OUTPUT (MOSI). A connected part
	responds by raising SPI_IRQ_INPUT, whose value is latched as the received
	byte. In client mode a byte arriving on SPI_IRQ_INPUT is latched and the
	current transmit byte is echoed back on SPI_IRQ_OUTPUT.

	Normal mode behaves like the classic SPI IF/WRCOL model. Buffered mode
	implements a one-byte TX queue and a two-byte RX FIFO, plus RXCIF/TXCIF/DREIF
	and BUFOVF state with the corresponding INTCTRL gating.

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
#include "avr_spi_modern.h"
#include "sim_cycle_timers.h"

/* CTRLA */
#define ENABLE_bm	0x01
#define PRESC_gm	0x06
#define PRESC_gp	1
#define CLK2X_bm	0x10
#define MASTER_bm	0x20

/* CTRLB */
#define BUFWR_bm	0x40
#define BUFEN_bm	0x80

/* INTCTRL */
#define IE_bm		0x01
#define SSIE_bm		0x10
#define DREIE_bm	0x20
#define TXCIE_bm	0x40
#define RXCIE_bm	0x80

/* INTFLAGS (normal mode) */
#define BUFOVF_bm	0x01
#define SSIF_bm		0x10
#define DREIF_bm	0x20
#define TXCIF_bm	0x40
#define WRCOL_bm	0x40
#define RXCIF_bm	0x80
#define IF_bm		0x80

static inline uint8_t rd(avr_t *avr, avr_io_addr_t a) { return avr->data[a]; }
static void set_bits(avr_t *avr, avr_io_addr_t a, uint8_t m)
{
	avr_core_watch_write(avr, a, avr->data[a] | m);
}
static void clr_bits(avr_t *avr, avr_io_addr_t a, uint8_t m)
{
	avr_core_watch_write(avr, a, avr->data[a] & ~m);
}

static int spi_enabled(avr_spi_modern_t *p)
{
	return (rd(p->io.avr, p->r_ctrla) & ENABLE_bm) != 0;
}
static int spi_master(avr_spi_modern_t *p)
{
	return (rd(p->io.avr, p->r_ctrla) & MASTER_bm) != 0;
}
static int spi_buffered(avr_spi_modern_t *p)
{
	return (rd(p->io.avr, p->r_ctrlb) & BUFEN_bm) != 0;
}

/* Transfer duration in CPU cycles: (prescaler / CLK2X) * 8 bits. */
static uint32_t spi_xfer_cycles(avr_spi_modern_t *p)
{
	static const uint8_t div[4] = { 4, 16, 64, 128 };
	avr_t *avr = p->io.avr;
	uint32_t d = div[(rd(avr, p->r_ctrla) & PRESC_gm) >> PRESC_gp];
	if (rd(avr, p->r_ctrla) & CLK2X_bm)
		d >>= 1;
	return d * 8;
}

static avr_cycle_count_t
avr_spi_modern_xfer(struct avr_t *avr, avr_cycle_count_t when, void *param);

static void spi_update_interrupt(avr_spi_modern_t *p)
{
	avr_t *avr = p->io.avr;
	uint8_t intctrl = rd(avr, p->r_intctrl);
	uint8_t flags = rd(avr, p->r_intflags);
	int active;

	if (spi_buffered(p))
		active = !!(flags & intctrl & (SSIE_bm | DREIE_bm | TXCIE_bm | RXCIE_bm));
	else
		active = !!((flags & IF_bm) && (intctrl & IE_bm));

	if (active) {
		if (!p->vect.pending)
			avr_raise_interrupt(avr, &p->vect);
	} else if (p->vect.pending)
		avr_clear_interrupt(avr, &p->vect);
}

static void spi_push_rx(avr_spi_modern_t *p, uint8_t v)
{
	avr_t *avr = p->io.avr;
	if (p->rx_count >= 2) {
		set_bits(avr, p->r_intflags, BUFOVF_bm);
		return;
	}
	p->rx_fifo[(p->rx_head + p->rx_count) & 1] = v;
	p->rx_count++;
	if (p->rx_count == 1)
		avr_core_watch_write(avr, p->r_data, v);
	set_bits(avr, p->r_intflags, RXCIF_bm);
}

static void spi_pop_rx_to_data(avr_spi_modern_t *p)
{
	avr_t *avr = p->io.avr;
	uint8_t next = 0;
	if (p->rx_count) {
		p->rx_head = (p->rx_head + 1) & 1;
		p->rx_count--;
		if (p->rx_count)
			next = p->rx_fifo[p->rx_head];
	}
	avr_core_watch_write(avr, p->r_data, next);
	if (p->rx_count)
		set_bits(avr, p->r_intflags, RXCIF_bm);
	else
		clr_bits(avr, p->r_intflags, RXCIF_bm);
	clr_bits(avr, p->r_intflags, BUFOVF_bm);
}

static void spi_start_host_transfer(avr_spi_modern_t *p, uint8_t byte)
{
	avr_t *avr = p->io.avr;
	p->busy = 1;
	p->tx_shift = byte;
	p->tx_shift_valid = 1;
	p->last_in = 0xff;
	avr_cycle_timer_register(avr, spi_xfer_cycles(p), avr_spi_modern_xfer, p);
}

static void spi_update_buffered_flags(avr_spi_modern_t *p, int xfer_complete)
{
	avr_t *avr = p->io.avr;
	uint8_t flags = rd(avr, p->r_intflags) & ~(DREIF_bm | TXCIF_bm);

	if (!p->tx_buf_valid)
		flags |= DREIF_bm;
	if (xfer_complete && !p->busy && !p->tx_buf_valid && !p->tx_shift_valid)
		flags |= TXCIF_bm;

	avr_core_watch_write(avr, p->r_intflags, flags);
	spi_update_interrupt(p);
}

/* Host transfer completes: emit MOSI and flag the result. */
static avr_cycle_count_t
avr_spi_modern_xfer(struct avr_t *avr, avr_cycle_count_t when, void *param)
{
	avr_spi_modern_t *p = (avr_spi_modern_t *)param;
	uint8_t out = p->tx_shift_valid ? p->tx_shift : rd(avr, p->r_data);

	p->busy = 0;
	if (!spi_enabled(p) || !spi_master(p))
		return 0;

	avr_raise_irq(p->io.irq + SPI_IRQ_OUTPUT, out);

	if (spi_buffered(p)) {
		spi_push_rx(p, p->last_in);
		if (p->tx_buf_valid) {
			uint8_t next = p->tx_buf;
			p->tx_buf_valid = 0;
			spi_update_buffered_flags(p, 0);
			spi_start_host_transfer(p, next);
			return 0;
		}
		p->tx_shift_valid = 0;
		spi_update_buffered_flags(p, 1);
		return 0;
	}

	avr_core_watch_write(avr, p->r_data, p->last_in);
	set_bits(avr, p->r_intflags, IF_bm);
	spi_update_interrupt(p);
	return 0;
}

static void
avr_spi_modern_data_write(struct avr_t *avr, avr_io_addr_t addr,
						  uint8_t v, void *param)
{
	avr_spi_modern_t *p = (avr_spi_modern_t *)param;

	if (!spi_enabled(p)) {
		avr_core_watch_write(avr, addr, v);
		return;
	}

	if (spi_buffered(p)) {
		clr_bits(avr, p->r_intflags, TXCIF_bm);
		if (!p->busy && !p->tx_shift_valid) {
			spi_update_buffered_flags(p, 0);
			if (spi_master(p))
				spi_start_host_transfer(p, v);
			else {
				p->tx_shift = v;
				p->tx_shift_valid = 1;
			}
			return;
		}
		if (!p->tx_buf_valid) {
			p->tx_buf = v;
			p->tx_buf_valid = 1;
			spi_update_buffered_flags(p, 0);
		}
		return;
	}

	/* Writing DATA while a transfer is in flight is a write collision. */
	if (p->busy) {
		set_bits(avr, p->r_intflags, WRCOL_bm);
		spi_update_interrupt(p);
		return;
	}
	avr_core_watch_write(avr, addr, v);

	if (spi_master(p))
		spi_start_host_transfer(p, v);
}

static uint8_t
avr_spi_modern_data_read(struct avr_t *avr, avr_io_addr_t addr, void *param)
{
	avr_spi_modern_t *p = (avr_spi_modern_t *)param;
	uint8_t v;

	if (spi_buffered(p)) {
		v = rd(avr, addr);
		if (p->rx_count)
			spi_pop_rx_to_data(p);
		else {
			clr_bits(avr, p->r_intflags, RXCIF_bm | BUFOVF_bm);
			spi_update_interrupt(p);
		}
		return v;
	}

	v = avr->data[addr];
	if (rd(avr, p->r_intflags) & IF_bm) {
		clr_bits(avr, p->r_intflags, IF_bm);
	}
	return v;
}

static void
avr_spi_modern_intctrl_write(struct avr_t *avr, avr_io_addr_t addr,
							 uint8_t v, void *param)
{
	avr_spi_modern_t *p = (avr_spi_modern_t *)param;
	avr_core_watch_write(avr, addr, v);
	spi_update_interrupt(p);
}

static void
avr_spi_modern_ctrlb_write(struct avr_t *avr, avr_io_addr_t addr,
						   uint8_t v, void *param)
{
	avr_spi_modern_t *p = (avr_spi_modern_t *)param;
	uint8_t old = rd(avr, addr);

	avr_core_watch_write(avr, addr, v);
	if (!!(old & BUFEN_bm) != !!(v & BUFEN_bm)) {
		/* Tear down any in-flight transfer so it cannot complete later under
		 * the new mode and leak a stale OUTPUT/interrupt across the boundary. */
		avr_cycle_timer_cancel(avr, avr_spi_modern_xfer, p);
		p->busy = 0;
		p->tx_shift_valid = 0;
		p->tx_buf_valid = 0;
		p->rx_head = 0;
		p->rx_count = 0;
		avr_core_watch_write(avr, p->r_intflags, 0);
		if (v & BUFEN_bm)
			spi_update_buffered_flags(p, 0);
		else
			spi_update_interrupt(p);
	}
}

static void
avr_spi_modern_intflags_write(struct avr_t *avr, avr_io_addr_t addr,
							  uint8_t v, void *param)
{
	avr_spi_modern_t *p = (avr_spi_modern_t *)param;
	uint8_t res = avr->data[addr] & ~v;
	avr_core_watch_write(avr, addr, res);
	if (spi_buffered(p) && (v & BUFOVF_bm))
		clr_bits(avr, p->r_intflags, BUFOVF_bm);
	spi_update_interrupt(p);
}

/* A byte arriving from the bus (client receive, or host's MISO reply). */
static void
avr_spi_modern_irq_input(struct avr_irq_t *irq, uint32_t value, void *param)
{
	avr_spi_modern_t *p = (avr_spi_modern_t *)param;
	avr_t *avr = p->io.avr;

	if (!spi_enabled(p))
		return;

	if (spi_master(p)) {
		p->last_in = value & 0xff;
	} else {
		uint8_t out = p->tx_shift_valid ? p->tx_shift : rd(avr, p->r_data);
		if (spi_buffered(p)) {
			spi_push_rx(p, value & 0xff);
			if (p->tx_buf_valid) {
				p->tx_shift = p->tx_buf;
				p->tx_shift_valid = 1;
				p->tx_buf_valid = 0;
				spi_update_buffered_flags(p, 0);
			} else {
				p->tx_shift_valid = 0;
				spi_update_buffered_flags(p, 1);
			}
			if (!p->rx_count)
				avr_core_watch_write(avr, p->r_data, value & 0xff);
			else if (p->rx_count == 1)
				avr_core_watch_write(avr, p->r_data, p->rx_fifo[p->rx_head]);
		} else {
			avr_core_watch_write(avr, p->r_data, value & 0xff);
			set_bits(avr, p->r_intflags, IF_bm);
			spi_update_interrupt(p);
		}
		avr_raise_irq(p->io.irq + SPI_IRQ_OUTPUT, out);
	}
}

static void
avr_spi_modern_reset(avr_io_t *io)
{
	avr_spi_modern_t *p = (avr_spi_modern_t *)io;
	avr_cycle_timer_cancel(p->io.avr, avr_spi_modern_xfer, p);
	p->busy = 0;
	p->tx_shift_valid = 0;
	p->tx_buf_valid = 0;
	p->rx_head = 0;
	p->rx_count = 0;
	p->last_in = 0xff;
}

static const char *irq_names[SPI_IRQ_COUNT] = {
	[SPI_IRQ_INPUT] = "8<in",
	[SPI_IRQ_OUTPUT] = "8>out",
};

static avr_io_t _io = {
	.kind = "spi_modern",
	.reset = avr_spi_modern_reset,
	.irq_names = irq_names,
};

void
avr_spi_modern_init(
		avr_t * avr,
		avr_spi_modern_t * p,
		avr_io_addr_t base,
		uint8_t vector,
		char name)
{
	memset(p, 0, sizeof(*p));
	p->io = _io;
	p->name = name;
	p->base = base;
	p->r_ctrla = base + SPIMR_CTRLA;
	p->r_ctrlb = base + SPIMR_CTRLB;
	p->r_intctrl = base + SPIMR_INTCTRL;
	p->r_intflags = base + SPIMR_INTFLAGS;
	p->r_data = base + SPIMR_DATA;

	/* One vector number; normal mode uses IE/IF, buffered mode uses the
	 * contiguous high nibble enables / flags (SSIF..RXCIF). */
	p->vect.vector = vector;
	p->vect.enable.reg = p->r_intctrl;
	p->vect.enable.bit = 0;
	p->vect.enable.mask = 0xff;
	p->vect.raised.reg = 0;
	p->vect.raise_sticky = 1;

	avr_register_io(avr, &p->io);
	avr_register_vector(avr, &p->vect);

	avr_io_setirqs(&p->io, AVR_IOCTL_SPI_GETIRQ(name), SPI_IRQ_COUNT, NULL);
	avr_irq_register_notify(p->io.irq + SPI_IRQ_INPUT,
							avr_spi_modern_irq_input, p);

	avr_register_io_write(avr, p->r_ctrlb, avr_spi_modern_ctrlb_write, p);
	avr_register_io_write(avr, p->r_intctrl, avr_spi_modern_intctrl_write, p);
	avr_register_io_write(avr, p->r_data, avr_spi_modern_data_write, p);
	avr_register_io_read(avr, p->r_data, avr_spi_modern_data_read, p);
	avr_register_io_write(avr, p->r_intflags, avr_spi_modern_intflags_write, p);
}
