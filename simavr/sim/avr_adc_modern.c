/*
	avr_adc_modern.c

	Modern AVR ADC, single-shot conversions. See avr_adc_modern.h.

	Copyright 2024 simavr modern-AVR fork (garyPenhook)

	This file is part of simavr. GNU GPL v3 or later; see COPYING.
 */

#include <stddef.h>
#include "avr_adc_modern.h"

#define R(p, o)	((p)->r_base + AVR_ADCM_ ## o)

// COMMAND.STCONV starts a conversion of the MUXPOS channel
static void
avr_adcm_command_write(avr_t * avr, avr_io_addr_t addr, uint8_t v, void * param)
{
	avr_adcm_t * p = (avr_adcm_t *)param;

	if (!(v & AVR_ADCM_STCONV) || !(avr->data[R(p, CTRLA)] & AVR_ADCM_ENABLE)) {
		avr->data[addr] = v;
		return;
	}
	uint8_t ch = avr->data[R(p, MUXPOS)] & 0x1f;
	uint32_t mv = ch < AVR_ADCM_CHANNELS ? p->chan_mv[ch] : 0;
	uint32_t vref = p->vref_mv ? p->vref_mv : 5000;
	uint32_t maxc = (avr->data[R(p, CTRLA)] & AVR_ADCM_RESSEL) ? 255 : 1023;

	uint32_t res = (mv * (maxc + 1)) / vref;
	if (res > maxc)
		res = maxc;
	avr->data[R(p, RES)] = res & 0xff;
	avr->data[R(p, RES) + 1] = (res >> 8) & 0xff;

	avr->data[addr] = v & ~AVR_ADCM_STCONV;	// conversion completes immediately
	avr->data[R(p, INTFLAGS)] |= AVR_ADCM_RESRDY;
	avr_raise_interrupt(avr, &p->resrdy);
}

static void
avr_adcm_intflags_write(avr_t * avr, avr_io_addr_t addr, uint8_t v, void * param)
{
	avr_adcm_t * p = (avr_adcm_t *)param;
	avr->data[addr] &= ~v;	// write-1-to-clear
	if (v & AVR_ADCM_RESRDY)
		avr_clear_interrupt(avr, &p->resrdy);
}

// a board/test presents the analog voltage (mV) on a channel
static void
avr_adcm_irq_input(struct avr_irq_t * irq, uint32_t value, void * param)
{
	avr_adcm_t * p = (avr_adcm_t *)param;
	if (irq->irq < AVR_ADCM_CHANNELS)
		p->chan_mv[irq->irq] = value;
}

static void
avr_adcm_reset(avr_io_t * io)
{
	avr_adcm_t * p = (avr_adcm_t *)io;
	for (int i = 0; i < AVR_ADCM_CHANNELS; i++)
		avr_irq_register_notify(p->io.irq + i, avr_adcm_irq_input, p);
}

static int
avr_adcm_ioctl(avr_io_t * io, uint32_t ctl, void * io_param)
{
	avr_adcm_t * p = (avr_adcm_t *)io;
	if (ctl == AVR_IOCTL_ADCM_GETIRQ(p->name)) {
		*(avr_irq_t **)io_param = p->io.irq;
		return 0;
	}
	return -1;
}

static const char * avr_adcm_irq_names[AVR_ADCM_IRQ_COUNT] = {
	"<ain0", "<ain1", "<ain2", "<ain3", "<ain4", "<ain5", "<ain6", "<ain7",
	"<ain8", "<ain9", "<ain10", "<ain11", "<ain12", "<ain13", "<ain14", "<ain15",
};

void
avr_adcm_init(avr_t * avr, avr_adcm_t * p)
{
	p->io.kind = "adc";
	p->io.reset = avr_adcm_reset;
	p->io.ioctl = avr_adcm_ioctl;
	p->io.irq_names = avr_adcm_irq_names;
	avr_register_io(avr, &p->io);
	avr_io_setirqs(&p->io, AVR_IOCTL_ADCM_GETIRQ(p->name),
			AVR_ADCM_IRQ_COUNT, NULL);
	if (p->resrdy.vector)
		avr_register_vector(avr, &p->resrdy);

	avr_register_io_write(avr, R(p, COMMAND), avr_adcm_command_write, p);
	avr_register_io_write(avr, R(p, INTFLAGS), avr_adcm_intflags_write, p);
}
