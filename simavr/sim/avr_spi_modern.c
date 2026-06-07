/*
	avr_spi_modern.c

	"Modern" AVR (AVRxt) register-block SPI, normal (non-buffered) mode. See
	avr_spi_modern.h.

	Wire model (identical to the classic avr_spi.c, synchronous): in host mode,
	writing DATA schedules a transfer of (prescaler * 8) CPU cycles; on
	completion the byte is emitted on SPI_IRQ_OUTPUT (MOSI) and the interrupt
	flag IF is set (SPIn_INT raised if INTCTRL.IE). A connected part responds by
	raising SPI_IRQ_INPUT, whose value is latched into DATA as the received byte
	(also setting IF). In client mode a byte arriving on SPI_IRQ_INPUT is latched
	and the current DATA echoed back on SPI_IRQ_OUTPUT. IF is cleared by reading
	DATA. Writing DATA mid-transfer sets WRCOL and is ignored.

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

/* INTCTRL */
#define IE_bm		0x01

/* INTFLAGS (normal mode) */
#define WRCOL_bm	0x40
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

/* Set IF and raise SPIn_INT (gated by INTCTRL.IE via the vector's enable). */
static void spi_flag_if(avr_spi_modern_t *p)
{
	avr_t *avr = p->io.avr;
	set_bits(avr, p->r_intflags, IF_bm);
	avr_raise_interrupt(avr, &p->vect);
}

/* Host transfer completes: emit MOSI and flag the result. */
static avr_cycle_count_t
avr_spi_modern_xfer(struct avr_t *avr, avr_cycle_count_t when, void *param)
{
	avr_spi_modern_t *p = (avr_spi_modern_t *)param;

	p->busy = 0;
	if (!spi_enabled(p) || !spi_master(p))
		return 0;

	/* Emit the byte first; a connected client may synchronously answer on INPUT,
	 * latching its MISO reply into DATA. Then flag completion exactly once (the
	 * master-side INPUT handler intentionally does not raise IF itself). */
	avr_raise_irq(p->io.irq + SPI_IRQ_OUTPUT, rd(avr, p->r_data));
	spi_flag_if(p);
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
	/* Writing DATA while a transfer is in flight is a write collision. */
	if (p->busy) {
		set_bits(avr, p->r_intflags, WRCOL_bm);
		return;
	}
	avr_core_watch_write(avr, addr, v);

	if (spi_master(p)) {
		p->busy = 1;
		avr_cycle_timer_register(avr, spi_xfer_cycles(p),
								 avr_spi_modern_xfer, p);
	}
	/* In client mode the byte just sits in DATA until the host clocks it. */
}

static uint8_t
avr_spi_modern_data_read(struct avr_t *avr, avr_io_addr_t addr, void *param)
{
	avr_spi_modern_t *p = (avr_spi_modern_t *)param;
	uint8_t v = avr->data[addr];
	/* Reading DATA clears IF (normal-mode "read INTFLAGS then access DATA"). */
	if (rd(avr, p->r_intflags) & IF_bm) {
		clr_bits(avr, p->r_intflags, IF_bm);
		avr_clear_interrupt(avr, &p->vect);
	}
	return v;
}

static void
avr_spi_modern_intflags_write(struct avr_t *avr, avr_io_addr_t addr,
							  uint8_t v, void *param)
{
	avr_spi_modern_t *p = (avr_spi_modern_t *)param;
	/* Buffered-mode flags are W1C; WRCOL is cleared this way too. IF follows
	 * the read-DATA path but accept a W1C here as well for robustness. */
	uint8_t res = avr->data[addr] & ~v;
	avr_core_watch_write(avr, addr, res);
	if (!(res & IF_bm))
		avr_clear_interrupt(avr, &p->vect);
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
		/* MISO reply to the byte we clocked out: latch it as received. IF is
		 * raised once by avr_spi_modern_xfer() after this returns, so do not
		 * flag it here (avoids a double interrupt per transfer). */
		avr_core_watch_write(avr, p->r_data, value & 0xff);
	} else {
		/* Client: latch the received byte and echo the current DATA back. */
		uint8_t out = rd(avr, p->r_data);
		avr_core_watch_write(avr, p->r_data, value & 0xff);
		spi_flag_if(p);
		avr_raise_irq(p->io.irq + SPI_IRQ_OUTPUT, out);
	}
}

static void
avr_spi_modern_reset(avr_io_t *io)
{
	avr_spi_modern_t *p = (avr_spi_modern_t *)io;
	avr_cycle_timer_cancel(p->io.avr, avr_spi_modern_xfer, p);
	p->busy = 0;
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

	/* Normal mode: IF (INTFLAGS bit7) enabled by INTCTRL.IE (bit0). */
	p->vect.vector = vector;
	p->vect.enable.reg = p->r_intctrl;
	p->vect.enable.bit = 0;		/* IE */
	p->vect.enable.mask = 1;
	p->vect.raised.reg = p->r_intflags;
	p->vect.raised.bit = 7;		/* IF */
	p->vect.raised.mask = 1;
	p->vect.raise_sticky = 1;

	avr_register_io(avr, &p->io);
	avr_register_vector(avr, &p->vect);

	avr_io_setirqs(&p->io, AVR_IOCTL_SPI_GETIRQ(name), SPI_IRQ_COUNT, NULL);
	avr_irq_register_notify(p->io.irq + SPI_IRQ_INPUT,
							avr_spi_modern_irq_input, p);

	avr_register_io_write(avr, p->r_data, avr_spi_modern_data_write, p);
	avr_register_io_read(avr, p->r_data, avr_spi_modern_data_read, p);
	avr_register_io_write(avr, p->r_intflags, avr_spi_modern_intflags_write, p);
}
