/*
	avr_usart.c

	Modern AVR USART. See avr_usart.h.

	Copyright 2024 simavr modern-AVR fork (garyPenhook)

	This file is part of simavr. GNU GPL v3 or later; see COPYING.
 */

#include <stdio.h>
#include "avr_usart.h"

#define R(p, o)	((p)->r_base + AVR_USART_ ## o)

// (re)evaluate the level-sensitive interrupt conditions
static void
avr_usart_update_irqs(avr_t * avr, avr_usart_t * p)
{
	uint8_t st = avr->data[R(p, STATUS)];
	uint8_t ca = avr->data[R(p, CTRLA)];

	if ((ca & AVR_USART_DREIE) && (st & AVR_USART_DREIF))
		avr_raise_interrupt(avr, &p->dre);
	if ((ca & AVR_USART_TXCIE) && (st & AVR_USART_TXCIF))
		avr_raise_interrupt(avr, &p->txc);
	if ((ca & AVR_USART_RXCIE) && (st & AVR_USART_RXCIF))
		avr_raise_interrupt(avr, &p->rxc);
}

// firmware writes a byte to transmit
static void
avr_usart_txdata_write(avr_t * avr, avr_io_addr_t addr, uint8_t v, void * param)
{
	avr_usart_t * p = (avr_usart_t *)param;

	avr->data[addr] = v;
	if (!(avr->data[R(p, CTRLB)] & AVR_USART_TXEN))
		return;
	// emit the byte; TX is modelled as immediate
	avr_raise_irq(p->io.irq + AVR_USART_IRQ_OUTPUT, v);
	// data register is empty again and the transfer is "complete"
	avr->data[R(p, STATUS)] |= AVR_USART_DREIF | AVR_USART_TXCIF;
	avr_usart_update_irqs(avr, p);
}

// STATUS: TXCIF is cleared by writing a 1 to it; DREIF/RXCIF are read-only here
static void
avr_usart_status_write(avr_t * avr, avr_io_addr_t addr, uint8_t v, void * param)
{
	avr_usart_t * p = (avr_usart_t *)param;
	if (v & AVR_USART_TXCIF) {
		avr->data[addr] &= ~AVR_USART_TXCIF;
		avr_clear_interrupt(avr, &p->txc);
	}
}

static void
avr_usart_ctrla_write(avr_t * avr, avr_io_addr_t addr, uint8_t v, void * param)
{
	avr->data[addr] = v;
	avr_usart_update_irqs(avr, (avr_usart_t *)param);
}

// reading RXDATAL consumes the byte and clears RXCIF
static uint8_t
avr_usart_rxdata_read(avr_t * avr, avr_io_addr_t addr, void * param)
{
	avr_usart_t * p = (avr_usart_t *)param;
	uint8_t v = avr->data[addr];
	avr->data[R(p, STATUS)] &= ~AVR_USART_RXCIF;
	avr_clear_interrupt(avr, &p->rxc);
	return v;
}

// a byte arrives from outside (host/board)
static void
avr_usart_irq_input(struct avr_irq_t * irq, uint32_t value, void * param)
{
	avr_usart_t * p = (avr_usart_t *)param;
	avr_t * avr = p->io.avr;

	if (!(avr->data[R(p, CTRLB)] & AVR_USART_RXEN))
		return;
	avr->data[R(p, RXDATAL)] = value & 0xff;
	avr->data[R(p, STATUS)] |= AVR_USART_RXCIF;
	avr_usart_update_irqs(avr, p);
}

static void
avr_usart_reset(avr_io_t * io)
{
	avr_usart_t * p = (avr_usart_t *)io;
	avr_t * avr = p->io.avr;
	// data register starts empty (ready to transmit)
	avr->data[R(p, STATUS)] = AVR_USART_DREIF;
	avr_irq_register_notify(p->io.irq + AVR_USART_IRQ_INPUT,
			avr_usart_irq_input, p);
}

static int
avr_usart_ioctl(avr_io_t * io, uint32_t ctl, void * io_param)
{
	avr_usart_t * p = (avr_usart_t *)io;
	if (ctl == AVR_IOCTL_USART_GETIRQ(p->name)) {
		*(avr_irq_t **)io_param = p->io.irq;
		return 0;
	}
	return -1;
}

static const char * avr_usart_irq_names[AVR_USART_IRQ_COUNT] = {
	[AVR_USART_IRQ_INPUT] = "8<in", [AVR_USART_IRQ_OUTPUT] = "8>out",
};

void
avr_usart_init(avr_t * avr, avr_usart_t * p)
{
	p->io.kind = "usart";
	p->io.reset = avr_usart_reset;
	p->io.ioctl = avr_usart_ioctl;
	p->io.irq_names = avr_usart_irq_names;
	avr_register_io(avr, &p->io);
	avr_io_setirqs(&p->io,
			AVR_IOCTL_USART_GETIRQ(p->name), AVR_USART_IRQ_COUNT, NULL);

	if (p->rxc.vector) avr_register_vector(avr, &p->rxc);
	if (p->dre.vector) avr_register_vector(avr, &p->dre);
	if (p->txc.vector) avr_register_vector(avr, &p->txc);

	avr_register_io_write(avr, R(p, TXDATAL), avr_usart_txdata_write, p);
	avr_register_io_write(avr, R(p, STATUS), avr_usart_status_write, p);
	avr_register_io_write(avr, R(p, CTRLA), avr_usart_ctrla_write, p);
	avr_register_io_read(avr, R(p, RXDATAL), avr_usart_rxdata_read, p);
}
