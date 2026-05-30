/*
	avr_spi_modern.c

	Modern AVR SPI, normal master mode. See avr_spi_modern.h.

	Copyright 2024 simavr modern-AVR fork (garyPenhook)

	This file is part of simavr. GNU GPL v3 or later; see COPYING.
 */

#include <stddef.h>
#include "avr_spi_modern.h"

#define R(p, o)	((p)->r_base + AVR_SPIM_ ## o)

// firmware writes DATA -> a master transfer happens
static void
avr_spim_data_write(avr_t * avr, avr_io_addr_t addr, uint8_t v, void * param)
{
	avr_spim_t * p = (avr_spim_t *)param;
	uint8_t ctrla = avr->data[R(p, CTRLA)];

	avr->data[addr] = v;
	if (!(ctrla & AVR_SPIM_ENABLE) || !(ctrla & AVR_SPIM_MASTER))
		return;

	// emit MOSI; a synchronous device hooked to OUTPUT may present its response
	// on the INPUT IRQ during this call (see avr_spim_irq_input)
	avr_raise_irq(p->io.irq + AVR_SPIM_IRQ_OUTPUT, v);
	// latch the received (MISO) byte and complete the transfer
	avr->data[addr] = p->miso;
	avr->data[R(p, INTFLAGS)] |= AVR_SPIM_IF;
	if (avr->data[R(p, INTCTRL)] & AVR_SPIM_IE)
		avr_raise_interrupt(avr, &p->spi);
}

// reading DATA clears the interrupt flag (normal mode)
static uint8_t
avr_spim_data_read(avr_t * avr, avr_io_addr_t addr, void * param)
{
	avr_spim_t * p = (avr_spim_t *)param;
	avr->data[R(p, INTFLAGS)] &= ~AVR_SPIM_IF;
	avr_clear_interrupt(avr, &p->spi);
	return avr->data[addr];
}

// a connected device presents its MISO byte
static void
avr_spim_irq_input(struct avr_irq_t * irq, uint32_t value, void * param)
{
	avr_spim_t * p = (avr_spim_t *)param;
	p->miso = value & 0xff;
}

static void
avr_spim_reset(avr_io_t * io)
{
	avr_spim_t * p = (avr_spim_t *)io;
	p->miso = 0xff;	// idle MISO line reads as 1s
	avr_irq_register_notify(p->io.irq + AVR_SPIM_IRQ_INPUT, avr_spim_irq_input, p);
}

static int
avr_spim_ioctl(avr_io_t * io, uint32_t ctl, void * io_param)
{
	avr_spim_t * p = (avr_spim_t *)io;
	if (ctl == AVR_IOCTL_SPIM_GETIRQ(p->name)) {
		*(avr_irq_t **)io_param = p->io.irq;
		return 0;
	}
	return -1;
}

static const char * avr_spim_irq_names[AVR_SPIM_IRQ_COUNT] = {
	[AVR_SPIM_IRQ_INPUT] = "8<miso", [AVR_SPIM_IRQ_OUTPUT] = "8>mosi",
};

void
avr_spim_init(avr_t * avr, avr_spim_t * p)
{
	p->io.kind = "spi";
	p->io.reset = avr_spim_reset;
	p->io.ioctl = avr_spim_ioctl;
	p->io.irq_names = avr_spim_irq_names;
	avr_register_io(avr, &p->io);
	avr_io_setirqs(&p->io, AVR_IOCTL_SPIM_GETIRQ(p->name),
			AVR_SPIM_IRQ_COUNT, NULL);
	if (p->spi.vector)
		avr_register_vector(avr, &p->spi);

	avr_register_io_write(avr, R(p, DATA), avr_spim_data_write, p);
	avr_register_io_read(avr, R(p, DATA), avr_spim_data_read, p);
}
