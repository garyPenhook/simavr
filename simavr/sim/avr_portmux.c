/*
	avr_portmux.c

	"Modern" AVR (AVRxt) Port Multiplexer — configuration store. See
	avr_portmux.h.

	The CTRL registers store what firmware writes and reset to 0. Routed
	peripheral outputs (see avr_portmux.h) are re-applied to their PORT pins on
	every register write.

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

#include <string.h>
#include "avr_portmux.h"

static avr_port_modern_t *
portmux_port(avr_portmux_t *p, avr_portmux_pin_t pin)
{
	if (pin.port < 'A' || pin.port > 'Z')
		return NULL;
	return p->ports[pin.port - 'A'];
}

static avr_portmux_pin_t
portmux_target(avr_portmux_t *p, const avr_portmux_func_t *f)
{
	static const avr_portmux_pin_t none = { 0, 0 };
	uint8_t sel = (p->io.avr->data[p->base + f->reg] & f->mask) >> f->shift;
	if (sel >= AVR_PORTMUX_MAX_OPTS || !portmux_port(p, f->pins[sel]))
		return none;
	return f->pins[sel];
}

static int
portmux_same(avr_portmux_pin_t a, avr_portmux_pin_t b)
{
	return a.port == b.port && (a.port == 0 || a.pin == b.pin);
}

/*
 * Bring the PORT overrides in line with every function's active flag, value and
 * the current routing registers. Pins that lost their function are released
 * first, so a pin that moved from one function to another ends up claimed.
 */
static void
portmux_apply(avr_portmux_t *p)
{
	for (int i = 0; i < p->nfuncs; i++) {
		avr_portmux_func_t *f = &p->funcs[i];
		avr_portmux_pin_t t = f->active ? portmux_target(p, f)
			: (avr_portmux_pin_t){ 0, 0 };
		if (f->claimed.port && !portmux_same(f->claimed, t)) {
			avr_port_modern_override(portmux_port(p, f->claimed),
									 f->claimed.pin, 0, 0, 0);
			f->claimed = (avr_portmux_pin_t){ 0, 0 };
		}
	}
	for (int i = 0; i < p->nfuncs; i++) {
		avr_portmux_func_t *f = &p->funcs[i];
		avr_portmux_pin_t t = f->active ? portmux_target(p, f)
			: (avr_portmux_pin_t){ 0, 0 };
		if (!t.port)
			continue;
		avr_port_modern_override(portmux_port(p, t), t.pin, 1,
								 f->force_output, f->value);
		f->claimed = t;
	}
}

void
avr_portmux_set_port(avr_portmux_t * p, char port, avr_port_modern_t * pt)
{
	if (port >= 'A' && port <= 'Z')
		p->ports[port - 'A'] = pt;
}

int
avr_portmux_add_func(
		avr_portmux_t * p,
		uint8_t reg,
		uint8_t mask,
		uint8_t shift,
		const avr_portmux_pin_t * pins,
		int npins,
		int force_output)
{
	if (p->nfuncs >= AVR_PORTMUX_MAX_FUNCS)
		return -1;
	avr_portmux_func_t *f = &p->funcs[p->nfuncs];
	memset(f, 0, sizeof(*f));
	f->pm = p;
	f->index = p->nfuncs;
	f->reg = reg;
	f->mask = mask;
	f->shift = shift;
	f->force_output = !!force_output;
	for (int i = 0; i < npins && i < AVR_PORTMUX_MAX_OPTS; i++)
		f->pins[i] = pins[i];
	return p->nfuncs++;
}

void
avr_portmux_func_set(avr_portmux_t * p, int func, int active, int value)
{
	if (func < 0 || func >= p->nfuncs)
		return;
	avr_portmux_func_t *f = &p->funcs[func];
	if (f->active == !!active && f->value == !!value)
		return;
	f->active = !!active;
	f->value = !!value;
	portmux_apply(p);
}

static void
portmux_pin_notify(struct avr_irq_t *irq, uint32_t value, void *param)
{
	avr_portmux_func_t *f = (avr_portmux_func_t *)param;
	(void)irq;
	avr_portmux_func_set(f->pm, f->index, (value >> 1) & 1, value & 1);
}

void
avr_portmux_bind_irq(avr_portmux_t * p, int func, struct avr_irq_t * irq)
{
	if (func >= 0 && func < p->nfuncs)
		avr_irq_register_notify(irq, portmux_pin_notify, &p->funcs[func]);
}

static void
avr_portmux_write(struct avr_t *avr, avr_io_addr_t addr, uint8_t v, void *param)
{
	avr_portmux_t *p = (avr_portmux_t *)param;
	avr_core_watch_write(avr, addr, v);
	portmux_apply(p);	/* moves any active function to its newly selected pin */
}

static void
avr_portmux_reset(avr_io_t *io)
{
	avr_portmux_t *p = (avr_portmux_t *)io;
	avr_t *avr = p->io.avr;
	for (int i = 0; i < p->nregs; i++)
		avr->data[p->base + i] = 0;	/* default routing */
	/* The PORTs (reset by their own hooks) have dropped their overrides; the
	 * peripherals reset to inactive too, so forget every claim. */
	for (int i = 0; i < p->nfuncs; i++) {
		p->funcs[i].active = p->funcs[i].value = 0;
		p->funcs[i].claimed = (avr_portmux_pin_t){ 0, 0 };
	}
}

static const char *irq_names[1] = { NULL };

static avr_io_t _io = {
	.kind = "portmux",
	.reset = avr_portmux_reset,
	.irq_names = irq_names,
};

void
avr_portmux_init_n(
		avr_t * avr,
		avr_portmux_t * p,
		avr_io_addr_t base,
		uint8_t nregs,
		char name)
{
	memset(p, 0, sizeof(*p));
	p->io = _io;
	p->name = name;
	p->base = base;
	p->nregs = nregs > AVR_PORTMUX_REGS_MAX ? AVR_PORTMUX_REGS_MAX : nregs;

	avr_register_io(avr, &p->io);
	for (int i = 0; i < p->nregs; i++)
		avr_register_io_write(avr, base + i, avr_portmux_write, p);
}

void
avr_portmux_init(
		avr_t * avr,
		avr_portmux_t * p,
		avr_io_addr_t base,
		char name)
{
	avr_portmux_init_n(avr, p, base, AVR_PORTMUX_REGS, name);
}
