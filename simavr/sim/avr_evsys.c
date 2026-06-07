/*
	avr_evsys.c

	"Modern" AVR (AVRxt) Event System — routing fabric. See avr_evsys.h.

	Channels (ch0/1 = SYNCCH0/1, ch2..5 = ASYNCCH0..3) hold a level driven by the
	CHn IRQ or pulsed by the strobe registers. Each user register selects a
	channel (value v -> channel v-1, 0 = off); whenever a routed channel changes
	(or a user is re-routed) the user's current value is emitted on its OUT IRQ.

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
#include "avr_evsys.h"

static inline uint8_t rd(avr_t *avr, avr_io_addr_t a) { return avr->data[a]; }

/* Channel a user is routed to, or -1 if off / out of range. */
static int user_channel(avr_evsys_t *p, int u)
{
	uint8_t sel = rd(p->io.avr, p->r_user[u]);
	if (sel >= 1 && sel <= p->nchannels)
		return sel - 1;
	return -1;
}

/* Emit 'level' to every user routed to channel 'ch'. */
static void evsys_propagate(avr_evsys_t *p, int ch, uint8_t level)
{
	for (int u = 0; u < p->nusers; u++)
		if (user_channel(p, u) == ch)
			avr_raise_irq(p->io.irq + AVR_EVSYS_IRQ_USER0 + u, level);
}

/* Software strobe: pulse the channel (1 then 0) to its routed users. */
static void evsys_strobe(avr_evsys_t *p, int ch)
{
	evsys_propagate(p, ch, 1);
	evsys_propagate(p, ch, 0);
}

static void
avr_evsys_strobe_write(struct avr_t *avr, avr_io_addr_t addr, uint8_t v,
					   void *param)
{
	avr_evsys_t *p = (avr_evsys_t *)param;
	avr_core_watch_write(avr, addr, v);
	for (int s = 0; s < p->nstrobe; s++) {
		if (p->r_strobe[s] != addr)
			continue;
		for (int k = 0; k < p->strobe_width[s]; k++)
			if (v & (1 << k))
				evsys_strobe(p, p->strobe_first_channel[s] + k);
		break;
	}
}

/* Re-routing a user immediately delivers the selected channel's current level. */
static void
avr_evsys_user_write(struct avr_t *avr, avr_io_addr_t addr, uint8_t v,
					 void *param)
{
	avr_evsys_t *p = (avr_evsys_t *)param;
	avr_core_watch_write(avr, addr, v);

	int u = -1;
	for (int i = 0; i < p->nusers; i++)
		if (p->r_user[i] == addr) { u = i; break; }
	if (u < 0)
		return;

	int ch = user_channel(p, u);
	if (ch >= 0)
		avr_raise_irq(p->io.irq + AVR_EVSYS_IRQ_USER0 + u, p->chan[ch]);
}

/* A real generator fired: drive 'level' onto every configured channel whose
 * generator-select register holds 'gen_value'. */
void
avr_evsys_async_generator(avr_evsys_t *p, uint8_t gen_value, uint8_t level)
{
	avr_t *avr = p->io.avr;
	if (gen_value == 0)	/* OFF never matches a routed channel */
		return;
	level &= 1;
	for (int ch = 0; ch < p->nchannels; ch++) {
		if (!p->r_chan[ch] || rd(avr, p->r_chan[ch]) != gen_value)
			continue;
		if (level == p->chan[ch])
			continue;
		p->chan[ch] = level;
		evsys_propagate(p, ch, level);
	}
}

/* A generator (or a test) drives a channel level via its CHn IRQ. */
static void
avr_evsys_irq_input(struct avr_irq_t *irq, uint32_t value, void *param)
{
	avr_evsys_t *p = (avr_evsys_t *)param;
	int ch = irq->irq - p->base_irq;
	if (ch < 0 || ch >= p->nchannels)
		return;
	uint8_t level = value & 1;
	if (level == p->chan[ch])
		return;
	p->chan[ch] = level;
	evsys_propagate(p, ch, level);
}

static void
avr_evsys_reset(avr_io_t *io)
{
	avr_evsys_t *p = (avr_evsys_t *)io;
	memset(p->chan, 0, sizeof(p->chan));
	for (int i = 0; i < p->nchannels; i++)
		if (p->r_chan[i])
			p->io.avr->data[p->r_chan[i]] = 0;
	for (int i = 0; i < p->nusers; i++)
		if (p->r_user[i])
			p->io.avr->data[p->r_user[i]] = 0;
	for (int i = 0; i < p->nstrobe; i++)
		if (p->r_strobe[i])
			p->io.avr->data[p->r_strobe[i]] = 0;
}

/* All entries must be non-NULL: avr_io_setirqs() dereferences each name. */
static const char *irq_names[AVR_EVSYS_IRQ_COUNT] = {
	"evsys.ch0", "evsys.ch1", "evsys.ch2", "evsys.ch3", "evsys.ch4", "evsys.ch5",
	"evsys.ch6", "evsys.ch7",
	">evsys.u0",  ">evsys.u1",  ">evsys.u2",  ">evsys.u3",  ">evsys.u4",
	">evsys.u5",  ">evsys.u6",  ">evsys.u7",  ">evsys.u8",  ">evsys.u9",
	">evsys.u10", ">evsys.u11", ">evsys.u12", ">evsys.u13", ">evsys.u14",
	">evsys.u15", ">evsys.u16", ">evsys.u17", ">evsys.u18", ">evsys.u19",
	">evsys.u20", ">evsys.u21", ">evsys.u22", ">evsys.u23",
};

static avr_io_t _io = {
	.kind = "evsys",
	.reset = avr_evsys_reset,
	.irq_names = irq_names,
};

void
avr_evsys_init(
		avr_t * avr,
		avr_evsys_t * p,
		avr_io_addr_t base,
		char name)
{
	memset(p, 0, sizeof(*p));
	p->io = _io;
	p->name = name;
	p->base = base;
	p->nchannels = 6;
	p->nusers = 15;
	p->nstrobe = 2;
	p->r_strobe[0] = base + EVSYSR_ASYNCSTROBE;
	p->strobe_first_channel[0] = 2;
	p->strobe_width[0] = 4;
	p->r_strobe[1] = base + EVSYSR_SYNCSTROBE;
	p->strobe_first_channel[1] = 0;
	p->strobe_width[1] = 2;
	p->r_chan[0] = base + EVSYSR_SYNCCH0;
	p->r_chan[1] = base + EVSYSR_SYNCCH0 + 1;
	for (int i = 0; i < 4; i++)
		p->r_chan[2 + i] = base + EVSYSR_ASYNCCH0 + i;
	for (int i = 0; i < 13; i++)	/* ASYNCUSER0..12 */
		p->r_user[i] = base + EVSYSR_ASYNCUSER0 + i;
	p->r_user[13] = base + EVSYSR_SYNCUSER0;
	p->r_user[14] = base + EVSYSR_SYNCUSER0 + 1;

	avr_register_io(avr, &p->io);
	avr_io_setirqs(&p->io, AVR_IOCTL_EVSYS_GETIRQ(name),
				   AVR_EVSYS_IRQ_COUNT, NULL);
	p->base_irq = p->io.irq[0].irq;
	for (int i = 0; i < p->nchannels; i++)
		avr_irq_register_notify(p->io.irq + i, avr_evsys_irq_input, p);

	for (int i = 0; i < p->nstrobe; i++)
		avr_register_io_write(avr, p->r_strobe[i], avr_evsys_strobe_write, p);
	for (int i = 0; i < p->nusers; i++)
		avr_register_io_write(avr, p->r_user[i], avr_evsys_user_write, p);
}

void
avr_evsys_init_mega(
		avr_t * avr,
		avr_evsys_t * p,
		avr_io_addr_t base,
		char name)
{
	memset(p, 0, sizeof(*p));
	p->io = _io;
	p->name = name;
	p->base = base;
	p->nchannels = 6;
	p->nusers = 24;
	p->nstrobe = 1;
	p->r_strobe[0] = base + 0x00;
	p->strobe_first_channel[0] = 0;
	p->strobe_width[0] = 6;
	for (int i = 0; i < p->nchannels; i++)
		p->r_chan[i] = base + 0x10 + i;
	for (int i = 0; i < p->nusers; i++)
		p->r_user[i] = base + 0x20 + i;

	avr_register_io(avr, &p->io);
	avr_io_setirqs(&p->io, AVR_IOCTL_EVSYS_GETIRQ(name),
				   AVR_EVSYS_IRQ_COUNT, NULL);
	p->base_irq = p->io.irq[0].irq;
	for (int i = 0; i < p->nchannels; i++)
		avr_irq_register_notify(p->io.irq + i, avr_evsys_irq_input, p);

	avr_register_io_write(avr, p->r_strobe[0], avr_evsys_strobe_write, p);
	for (int i = 0; i < p->nusers; i++)
		avr_register_io_write(avr, p->r_user[i], avr_evsys_user_write, p);
}
