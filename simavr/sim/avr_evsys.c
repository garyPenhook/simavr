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
	if (sel >= 1 && sel <= AVR_EVSYS_CHANNELS)
		return sel - 1;
	return -1;
}

/* Emit 'level' to every user routed to channel 'ch'. */
static void evsys_propagate(avr_evsys_t *p, int ch, uint8_t level)
{
	for (int u = 0; u < AVR_EVSYS_USERS; u++)
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
avr_evsys_asyncstrobe_write(struct avr_t *avr, avr_io_addr_t addr, uint8_t v,
							void *param)
{
	avr_evsys_t *p = (avr_evsys_t *)param;
	avr_core_watch_write(avr, addr, v);
	for (int k = 0; k < 4; k++)	/* ASYNCSTROBE bit k -> ASYNCCHk = ch 2+k */
		if (v & (1 << k))
			evsys_strobe(p, 2 + k);
}

static void
avr_evsys_syncstrobe_write(struct avr_t *avr, avr_io_addr_t addr, uint8_t v,
						   void *param)
{
	avr_evsys_t *p = (avr_evsys_t *)param;
	avr_core_watch_write(avr, addr, v);
	for (int k = 0; k < 2; k++)	/* SYNCSTROBE bit k -> SYNCCHk = ch k */
		if (v & (1 << k))
			evsys_strobe(p, k);
}

/* Re-routing a user immediately delivers the selected channel's current level. */
static void
avr_evsys_user_write(struct avr_t *avr, avr_io_addr_t addr, uint8_t v,
					 void *param)
{
	avr_evsys_t *p = (avr_evsys_t *)param;
	avr_core_watch_write(avr, addr, v);

	int u = -1;
	for (int i = 0; i < AVR_EVSYS_USERS; i++)
		if (p->r_user[i] == addr) { u = i; break; }
	if (u < 0)
		return;

	int ch = user_channel(p, u);
	if (ch >= 0)
		avr_raise_irq(p->io.irq + AVR_EVSYS_IRQ_USER0 + u, p->chan[ch]);
}

/* A real generator fired: drive 'level' onto every async channel whose
 * generator-select register holds 'gen_value' (async channels = module ch 2..5,
 * which share one source encoding). */
void
avr_evsys_async_generator(avr_evsys_t *p, uint8_t gen_value, uint8_t level)
{
	avr_t *avr = p->io.avr;
	if (gen_value == 0)	/* OFF never matches a routed channel */
		return;
	level &= 1;
	for (int k = 0; k < 4; k++) {
		if (rd(avr, p->r_asyncch[k]) != gen_value)
			continue;
		int ch = 2 + k;		/* ASYNCCHk -> module channel 2+k */
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
	if (ch < 0 || ch >= AVR_EVSYS_CHANNELS)
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
}

/* All entries must be non-NULL: avr_io_setirqs() dereferences each name. */
static const char *irq_names[AVR_EVSYS_IRQ_COUNT] = {
	"evsys.ch0", "evsys.ch1", "evsys.ch2", "evsys.ch3", "evsys.ch4", "evsys.ch5",
	">evsys.u0",  ">evsys.u1",  ">evsys.u2",  ">evsys.u3",  ">evsys.u4",
	">evsys.u5",  ">evsys.u6",  ">evsys.u7",  ">evsys.u8",  ">evsys.u9",
	">evsys.u10", ">evsys.u11", ">evsys.u12", ">evsys.u13", ">evsys.u14",
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
	p->r_asyncstrobe = base + EVSYSR_ASYNCSTROBE;
	p->r_syncstrobe = base + EVSYSR_SYNCSTROBE;
	for (int i = 0; i < 4; i++)	/* ASYNCCH0..3 generator selects */
		p->r_asyncch[i] = base + EVSYSR_ASYNCCH0 + i;
	for (int i = 0; i < 13; i++)	/* ASYNCUSER0..12 */
		p->r_user[i] = base + EVSYSR_ASYNCUSER0 + i;
	p->r_user[13] = base + EVSYSR_SYNCUSER0;
	p->r_user[14] = base + EVSYSR_SYNCUSER0 + 1;

	avr_register_io(avr, &p->io);
	avr_io_setirqs(&p->io, AVR_IOCTL_EVSYS_GETIRQ(name),
				   AVR_EVSYS_IRQ_COUNT, NULL);
	p->base_irq = p->io.irq[0].irq;
	for (int i = 0; i < AVR_EVSYS_CHANNELS; i++)
		avr_irq_register_notify(p->io.irq + i, avr_evsys_irq_input, p);

	avr_register_io_write(avr, p->r_asyncstrobe, avr_evsys_asyncstrobe_write, p);
	avr_register_io_write(avr, p->r_syncstrobe, avr_evsys_syncstrobe_write, p);
	for (int i = 0; i < AVR_EVSYS_USERS; i++)
		avr_register_io_write(avr, p->r_user[i], avr_evsys_user_write, p);
}
