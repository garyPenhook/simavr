/*
	avr_twi_modern.c

	"Modern" AVR (AVRxt) Two-Wire Interface (TWI0), as found on the tinyAVR
	1-series (ATtiny3217), megaAVR-0 and AVR Dx families.

	This is a register front-end for the modern TWI peripheral that drives the
	same wire IRQ protocol as the classic avr_twi.c (TWI_IRQ_INPUT / _OUTPUT /
	_STATUS, carrying TWI_COND_* messages). That makes it bus-compatible with
	the existing simavr I2C parts (examples/parts/i2c_eeprom.c, ds1338, ...) and
	with the classic TWI peripheral: a modern master can drive a classic slave
	and vice-versa.

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
#include "avr_twi_modern.h"
#include "sim_regbit.h"

/* ------------------------------------------------------------------ *
 * Register bit layout (matches the device header TWI_*_bp positions)
 * ------------------------------------------------------------------ */

/* MCTRLA (host control A) */
#define M_ENABLE	(1 << 0)
#define M_SMEN		(1 << 1)
#define M_QCEN		(1 << 4)
#define M_WIEN		(1 << 6)
#define M_RIEN		(1 << 7)

/* MCTRLB (host control B) */
#define M_MCMD		0x03	/* command, bits 0-1 */
#define M_ACKACT	(1 << 2)
#define M_FLUSH		(1 << 3)

/* MCMD values */
#define MCMD_NOACT	0
#define MCMD_REPSTART	1
#define MCMD_RECVTRANS	2
#define MCMD_STOP	3

/* MSTATUS (host status) */
#define M_BUSSTATE	0x03	/* bits 0-1 */
#define M_BUSERR	(1 << 2)
#define M_ARBLOST	(1 << 3)
#define M_RXACK		(1 << 4)
#define M_CLKHOLD	(1 << 5)
#define M_WIF		(1 << 6)
#define M_RIF		(1 << 7)

/* BUSSTATE values */
#define BUS_UNKNOWN	0
#define BUS_IDLE	1
#define BUS_OWNER	2
#define BUS_BUSY	3

/* SCTRLA (client control A) */
#define S_ENABLE	(1 << 0)
#define S_SMEN		(1 << 1)
#define S_PMEN		(1 << 2)
#define S_PIEN		(1 << 5)
#define S_APIEN		(1 << 6)
#define S_DIEN		(1 << 7)

/* SCTRLB (client control B) */
#define S_SCMD		0x03	/* bits 0-1 */
#define S_ACKACT	(1 << 2)

/* SCMD values */
#define SCMD_NOACT	0
#define SCMD_COMPTRANS	2
#define SCMD_RESPONSE	3

/* SSTATUS (client status) */
#define S_AP		(1 << 0)	/* 1: address, 0: stop */
#define S_DIR		(1 << 1)	/* 1: master reading from us */
#define S_BUSERR	(1 << 2)
#define S_COLL		(1 << 3)
#define S_RXACK		(1 << 4)
#define S_CLKHOLD	(1 << 5)
#define S_APIF		(1 << 6)
#define S_DIF		(1 << 7)

/* SADDRMASK */
#define S_ADDREN	(1 << 0)

#define AVR_TWIM_DEBUG 0

static inline uint8_t rd(avr_t *avr, avr_io_addr_t a) { return avr->data[a]; }
static inline void wr(avr_t *avr, avr_io_addr_t a, uint8_t v)
{
	avr_core_watch_write(avr, a, v);
}
static inline void set_bits(avr_t *avr, avr_io_addr_t a, uint8_t m)
{
	avr_core_watch_write(avr, a, avr->data[a] | m);
}
static inline void clr_bits(avr_t *avr, avr_io_addr_t a, uint8_t m)
{
	avr_core_watch_write(avr, a, avr->data[a] & ~m);
}

/* ------------------------------------------------------------------ *
 * Wire helpers
 * ------------------------------------------------------------------ */

static inline void
twi_modern_send(avr_twi_modern_t *p, uint8_t cond, uint8_t addr, uint8_t data)
{
	avr_raise_irq(p->io.irq + TWI_IRQ_OUTPUT,
				  avr_twi_irq_msg(cond, addr, data));
}

/* ------------------------------------------------------------------ *
 * Master role
 * ------------------------------------------------------------------ */

/* Update the (read-only) RXACK bit and raise the host interrupt for a flag. */
static void
twi_modern_master_flag(avr_twi_modern_t *p, uint8_t flag)
{
	avr_t *avr = p->io.avr;
	uint8_t mctrla = rd(avr, p->r_mctrla);

	set_bits(avr, p->r_mstatus, flag | M_CLKHOLD);
	/* WIF is enabled by WIEN, RIF by RIEN. */
	if (((flag & M_WIF) && (mctrla & M_WIEN)) ||
		((flag & M_RIF) && (mctrla & M_RIEN)))
		avr_raise_interrupt(avr, &p->mvector);
}

/* Issue a START + address (called when MADDR is written). */
static void
twi_modern_master_start(avr_twi_modern_t *p, uint8_t maddr)
{
	avr_t *avr = p->io.avr;

	p->m_dir = maddr & 1;	/* 1 = read */
	p->m_ack = 0;
	p->m_rx_valid = 0;

	/* We own the bus now; clear stale flags. */
	clr_bits(avr, p->r_mstatus,
			 M_BUSSTATE | M_WIF | M_RIF | M_ARBLOST | M_BUSERR | M_RXACK);
	set_bits(avr, p->r_mstatus, BUS_OWNER);

	/* Put START+address on the wire; the addressed slave replies (synchronously
	 * in this model) with an ACK on our INPUT IRQ. */
	p->m_busy = 1;
	twi_modern_send(p, TWI_COND_START, maddr, 0);
	p->m_busy = 0;

	if (!p->m_ack)
		set_bits(avr, p->r_mstatus, M_RXACK);	/* address NACKed */

	if (p->m_dir == 0) {
		/* write transfer: address phase done, expect MDATA next */
		twi_modern_master_flag(p, M_WIF);
	} else {
		/* read transfer: if the slave ACKed, clock in the first byte */
		if (p->m_ack) {
			p->m_busy = 1;
			twi_modern_send(p, TWI_COND_READ, maddr, 0);
			p->m_busy = 0;
			if (p->m_rx_valid)
				wr(avr, p->r_mdata, p->m_rx_data);
			twi_modern_master_flag(p, M_RIF);
		} else {
			/* address NACKed on a read: report via WIF */
			twi_modern_master_flag(p, M_WIF);
		}
	}
}

static void
avr_twi_modern_maddr_write(struct avr_t *avr, avr_io_addr_t addr,
						   uint8_t v, void *param)
{
	avr_twi_modern_t *p = (avr_twi_modern_t *)param;

	avr_core_watch_write(avr, addr, v);
	if (!(rd(avr, p->r_mctrla) & M_ENABLE))
		return;
	twi_modern_master_start(p, v);
}

static void
avr_twi_modern_mdata_write(struct avr_t *avr, avr_io_addr_t addr,
						   uint8_t v, void *param)
{
	avr_twi_modern_t *p = (avr_twi_modern_t *)param;

	avr_core_watch_write(avr, addr, v);
	if (!(rd(avr, p->r_mctrla) & M_ENABLE))
		return;
	if (p->m_dir != 0)	/* only meaningful in a write transfer */
		return;

	clr_bits(avr, p->r_mstatus, M_WIF | M_RXACK);
	p->m_ack = 0;
	p->m_busy = 1;
	twi_modern_send(p, TWI_COND_WRITE, rd(avr, p->r_maddr), v);
	p->m_busy = 0;
	if (!p->m_ack)
		set_bits(avr, p->r_mstatus, M_RXACK);
	twi_modern_master_flag(p, M_WIF);
}

static uint8_t
avr_twi_modern_mdata_read(struct avr_t *avr, avr_io_addr_t addr, void *param)
{
	avr_twi_modern_t *p = (avr_twi_modern_t *)param;

	/* Reading data releases the clock hold and the read flag (non-smart-mode
	 * firmware then issues a command via MCTRLB). */
	clr_bits(avr, p->r_mstatus, M_RIF | M_CLKHOLD);
	if (!(rd(avr, p->r_mstatus) & (M_WIF | M_RIF)))
		avr_clear_interrupt(avr, &p->mvector);
	return avr->data[addr];
}

static void
avr_twi_modern_mctrlb_write(struct avr_t *avr, avr_io_addr_t addr,
							uint8_t v, void *param)
{
	avr_twi_modern_t *p = (avr_twi_modern_t *)param;
	uint8_t cmd = v & M_MCMD;
	uint8_t nack = (v & M_ACKACT) != 0;	/* ACKACT: 0 = ACK, 1 = NACK */

	/* MCMD is a strobe; don't latch it. ACKACT/FLUSH are normal bits. */
	avr_core_watch_write(avr, addr, v & ~M_MCMD);

	if (!(rd(avr, p->r_mctrla) & M_ENABLE))
		return;

	if (v & M_FLUSH) {
		/* reset the host: abort and go idle */
		clr_bits(avr, p->r_mstatus,
				 M_BUSSTATE | M_WIF | M_RIF | M_CLKHOLD | M_RXACK);
		set_bits(avr, p->r_mstatus, BUS_IDLE);
		avr_clear_interrupt(avr, &p->mvector);
		return;
	}

	switch (cmd) {
	case MCMD_REPSTART:
		/* repeated start to the current MADDR */
		twi_modern_master_start(p, rd(avr, p->r_maddr));
		break;
	case MCMD_RECVTRANS:
		if (p->m_dir != 0 && !nack) {
			/* ACK the previous byte and clock in the next one */
			clr_bits(avr, p->r_mstatus, M_RIF | M_CLKHOLD);
			p->m_rx_valid = 0;
			p->m_busy = 1;
			twi_modern_send(p, TWI_COND_READ, rd(avr, p->r_maddr), 0);
			p->m_busy = 0;
			if (p->m_rx_valid)
				wr(avr, p->r_mdata, p->m_rx_data);
			twi_modern_master_flag(p, M_RIF);
		}
		break;
	case MCMD_STOP:
		twi_modern_send(p, TWI_COND_STOP, rd(avr, p->r_maddr), nack ? 0 : 1);
		clr_bits(avr, p->r_mstatus, M_BUSSTATE | M_WIF | M_RIF | M_CLKHOLD);
		set_bits(avr, p->r_mstatus, BUS_IDLE);
		avr_clear_interrupt(avr, &p->mvector);
		break;
	default:
		break;
	}
}

static void
avr_twi_modern_mstatus_write(struct avr_t *avr, avr_io_addr_t addr,
							 uint8_t v, void *param)
{
	avr_twi_modern_t *p = (avr_twi_modern_t *)param;
	uint8_t cur = rd(avr, p->r_mstatus);
	uint8_t w1c = M_WIF | M_RIF | M_BUSERR | M_ARBLOST;	/* write-1-to-clear */
	uint8_t res = cur;

	/* Clear any W1C flag that is being written as 1. */
	res &= ~(v & w1c);
	/* BUSSTATE is writable (firmware forces IDLE before the first transfer). */
	res = (res & ~M_BUSSTATE) | (v & M_BUSSTATE);
	/* RXACK and CLKHOLD are read-only: keep current. */

	avr_core_watch_write(avr, addr, res);
	if (!(res & (M_WIF | M_RIF)))
		avr_clear_interrupt(avr, &p->mvector);
}

/* ------------------------------------------------------------------ *
 * Slave role
 * ------------------------------------------------------------------ */

static int
twi_modern_slave_match(avr_twi_modern_t *p, uint8_t wire_addr)
{
	avr_t *avr = p->io.avr;
	uint8_t saddr = rd(avr, p->r_saddr) >> 1;
	uint8_t got = wire_addr >> 1;
	uint8_t mask = rd(avr, p->r_saddrmask);
	uint8_t ignore = (mask & S_ADDREN) ? (mask >> 1) : 0;

	return (got & ~ignore) == (saddr & ~ignore);
}

static void
avr_twi_modern_irq_input(struct avr_irq_t *irq, uint32_t value, void *param)
{
	avr_twi_modern_t *p = (avr_twi_modern_t *)param;
	avr_t *avr = p->io.avr;
	avr_twi_msg_irq_t msg;

	msg.u.v = value;

	/* While a master wire op is in flight, the bus traffic we see is the
	 * addressed slave's reply to *us* (the master). */
	if (p->m_busy) {
		if (msg.u.twi.msg & TWI_COND_ACK)
			p->m_ack = msg.u.twi.data & 1;
		if (msg.u.twi.msg & TWI_COND_READ) {
			p->m_rx_data = msg.u.twi.data;
			p->m_rx_valid = 1;
		}
		return;
	}

	/* Otherwise we are acting as a slave: a remote master is driving us. */
	if (!(rd(avr, p->r_sctrla) & S_ENABLE))
		return;

	if (msg.u.twi.msg & TWI_COND_START) {
		p->s_selected = 0;
		if (msg.u.twi.msg & TWI_COND_ADDR) {
			if (twi_modern_slave_match(p, msg.u.twi.addr)) {
				uint8_t dir = msg.u.twi.addr & 1;	/* 1: master reads from us */
				p->s_selected = msg.u.twi.addr;
				clr_bits(avr, p->r_sstatus, S_DIR);
				set_bits(avr, p->r_sstatus,
						 S_AP | S_APIF | S_CLKHOLD | (dir ? S_DIR : 0));
				/* Auto-ACK the address so the synchronous master proceeds. */
				twi_modern_send(p, TWI_COND_ACK, p->s_selected, 1);
				if (rd(avr, p->r_sctrla) & S_APIEN)
					avr_raise_interrupt(avr, &p->svector);
			}
		}
	}

	if (p->s_selected) {
		if (msg.u.twi.msg & TWI_COND_WRITE) {
			/* remote master writes a byte to us */
			wr(avr, p->r_sdata, msg.u.twi.data);
			clr_bits(avr, p->r_sstatus, S_DIR);
			set_bits(avr, p->r_sstatus, S_DIF | S_CLKHOLD);
			twi_modern_send(p, TWI_COND_ACK, p->s_selected, 1);
			if (rd(avr, p->r_sctrla) & S_DIEN)
				avr_raise_interrupt(avr, &p->svector);
		}
		if (msg.u.twi.msg & TWI_COND_READ) {
			/* remote master reads a byte from us: reply with SDATA */
			uint8_t d = rd(avr, p->r_sdata);
			twi_modern_send(p, TWI_COND_READ, p->s_selected, d);
			set_bits(avr, p->r_sstatus, S_DIR | S_DIF | S_CLKHOLD);
			if (rd(avr, p->r_sctrla) & S_DIEN)
				avr_raise_interrupt(avr, &p->svector);
		}
	}

	if (msg.u.twi.msg & TWI_COND_STOP) {
		if (p->s_selected) {
			clr_bits(avr, p->r_sstatus, S_AP | S_CLKHOLD);
			set_bits(avr, p->r_sstatus, S_APIF);
			if (rd(avr, p->r_sctrla) & (S_APIEN | S_PIEN))
				avr_raise_interrupt(avr, &p->svector);
		}
		p->s_selected = 0;
	}
}

static void
avr_twi_modern_sctrlb_write(struct avr_t *avr, avr_io_addr_t addr,
							uint8_t v, void *param)
{
	avr_twi_modern_t *p = (avr_twi_modern_t *)param;
	uint8_t cmd = v & S_SCMD;

	/* SCMD is a strobe; don't latch it. */
	avr_core_watch_write(avr, addr, v & ~S_SCMD);

	if (!(rd(avr, p->r_sctrla) & S_ENABLE))
		return;

	/* In this synchronous model the wire-level ACK/data have already been
	 * exchanged, so a command just releases the held clock and the flags. */
	if (cmd == SCMD_COMPTRANS || cmd == SCMD_RESPONSE) {
		clr_bits(avr, p->r_sstatus, S_DIF | S_APIF | S_CLKHOLD);
		avr_clear_interrupt(avr, &p->svector);
	}
}

static void
avr_twi_modern_sstatus_write(struct avr_t *avr, avr_io_addr_t addr,
							 uint8_t v, void *param)
{
	avr_twi_modern_t *p = (avr_twi_modern_t *)param;
	uint8_t cur = rd(avr, p->r_sstatus);
	uint8_t w1c = S_APIF | S_DIF | S_BUSERR | S_COLL;
	uint8_t res = cur & ~(v & w1c);

	avr_core_watch_write(avr, addr, res);
	if (!(res & (S_APIF | S_DIF)))
		avr_clear_interrupt(avr, &p->svector);
}

/* ------------------------------------------------------------------ *
 * Module plumbing
 * ------------------------------------------------------------------ */

static void
avr_twi_modern_reset(struct avr_io_t *io)
{
	avr_twi_modern_t *p = (avr_twi_modern_t *)io;
	avr_t *avr = p->io.avr;

	p->m_dir = p->m_busy = p->m_ack = p->m_rx_valid = p->m_rx_data = 0;
	p->s_selected = 0;
	/* registers reset to 0 (BUSSTATE = UNKNOWN) by the core */
	avr->data[p->r_mstatus] = 0;
	avr->data[p->r_sstatus] = 0;
}

static const char *irq_names[TWI_IRQ_COUNT] = {
	[TWI_IRQ_INPUT] = "8<input",
	[TWI_IRQ_OUTPUT] = "32>output",
	[TWI_IRQ_STATUS] = "8>status",
};

static avr_io_t _io = {
	.kind = "twi_modern",
	.reset = avr_twi_modern_reset,
	.irq_names = irq_names,
};

void
avr_twi_modern_init(
		avr_t * avr,
		avr_twi_modern_t * p,
		avr_io_addr_t base,
		uint8_t mvector,
		uint8_t svector,
		char name)
{
	memset(p, 0, sizeof(*p));
	p->io = _io;
	p->name = name;
	p->base = base;

	p->r_ctrla = base + TWIM_CTRLA;
	p->r_dbgctrl = base + TWIM_DBGCTRL;
	p->r_mctrla = base + TWIM_MCTRLA;
	p->r_mctrlb = base + TWIM_MCTRLB;
	p->r_mstatus = base + TWIM_MSTATUS;
	p->r_mbaud = base + TWIM_MBAUD;
	p->r_maddr = base + TWIM_MADDR;
	p->r_mdata = base + TWIM_MDATA;
	p->r_sctrla = base + TWIM_SCTRLA;
	p->r_sctrlb = base + TWIM_SCTRLB;
	p->r_sstatus = base + TWIM_SSTATUS;
	p->r_saddr = base + TWIM_SADDR;
	p->r_sdata = base + TWIM_SDATA;
	p->r_saddrmask = base + TWIM_SADDRMASK;

	/*
	 * The host (TWIM) interrupt is requested by WIF (bit 6) or RIF (bit 7) of
	 * MSTATUS, enabled by WIEN/RIEN in MCTRLA. The client (TWIS) interrupt is
	 * requested by APIF (bit 6) / DIF (bit 7) of SSTATUS. We use multi-bit
	 * regbits so the engine's enable check sees "either" bit; the precise
	 * flag/enable pairing is enforced in the write handlers, and the flags are
	 * set/cleared there directly (so .raised is left unset to avoid the engine
	 * touching both bits at once). The flags are sticky (software-cleared).
	 */
	p->mvector.vector = mvector;
	p->mvector.enable.reg = p->r_mctrla;	/* WIEN(6) | RIEN(7) */
	p->mvector.enable.bit = 6;
	p->mvector.enable.mask = 0x03;
	p->mvector.raise_sticky = 1;

	p->svector.vector = svector;
	p->svector.enable.reg = p->r_sctrla;	/* PIEN(5) | APIEN(6) | DIEN(7) */
	p->svector.enable.bit = 5;
	p->svector.enable.mask = 0x07;
	p->svector.raise_sticky = 1;

	avr_register_io(avr, &p->io);
	avr_register_vector(avr, &p->mvector);
	avr_register_vector(avr, &p->svector);

	avr_io_setirqs(&p->io, AVR_IOCTL_TWI_GETIRQ(p->name), TWI_IRQ_COUNT, NULL);

	/* Bus traffic arrives on the INPUT IRQ (slave requests from a remote master,
	 * or a slave's replies to us while we are master). Registered here rather
	 * than in reset() so it is wired even when init() is called after the core's
	 * global reset (e.g. unit tests). */
	avr_irq_register_notify(p->io.irq + TWI_IRQ_INPUT,
							avr_twi_modern_irq_input, p);

	avr_register_io_write(avr, p->r_maddr, avr_twi_modern_maddr_write, p);
	avr_register_io_write(avr, p->r_mdata, avr_twi_modern_mdata_write, p);
	avr_register_io_read(avr, p->r_mdata, avr_twi_modern_mdata_read, p);
	avr_register_io_write(avr, p->r_mctrlb, avr_twi_modern_mctrlb_write, p);
	avr_register_io_write(avr, p->r_mstatus, avr_twi_modern_mstatus_write, p);

	avr_register_io_write(avr, p->r_sctrlb, avr_twi_modern_sctrlb_write, p);
	avr_register_io_write(avr, p->r_sstatus, avr_twi_modern_sstatus_write, p);
}
