/*
	avr_port_modern.c

	"Modern" AVR (AVRxt) PORT / VPORT peripheral. See avr_port_modern.h.

	Implementation notes
	--------------------
	The canonical state lives in the PORT block: DIR (base+0), OUT (base+4),
	IN (base+8), INTFLAGS (base+9). The SET/CLR/TGL registers are write strobes
	that modify DIR/OUT and read back the current DIR/OUT value (they are
	aliases, not separate storage). The VPORT block is wired up via the engine's
	low-I/O redirect table (avr->lowio_redirect) so VPORTx.DIR/OUT/IN/INTFLAGS —
	reachable by SBI/CBI — resolve to the PORT registers here.

	Pin interrupts use the per-pin PINnCTRL.ISC field (BOTHEDGES/RISING/FALLING/
	LEVEL); a triggered pin sets its INTFLAGS bit and raises PORTx_PORT.

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
#include "avr_port_modern.h"

/* PINnCTRL bits */
#define PINCTRL_ISC	0x07
#define PINCTRL_PULLUPEN	0x08
#define PINCTRL_INVEN	0x80

/* ISC (input/sense configuration) values */
#define ISC_INTDISABLE	0
#define ISC_BOTHEDGES	1
#define ISC_RISING	2
#define ISC_FALLING	3
#define ISC_INPUT_DISABLE 4
#define ISC_LEVEL	5

static inline uint8_t pin_mask_from_ctrl(avr_port_modern_t *p, uint8_t bit)
{
	avr_t *avr = p->io.avr;
	uint8_t m = 0;
	for (int i = 0; i < 8; i++)
		if (avr->data[p->r_pinctrl + i] & bit)
			m |= (1 << i);
	return m;
}

/*
 * Recompute the per-pin output IRQs and the aggregate PIN_ALL IRQ from DIR/OUT,
 * internal pull-ups (PINnCTRL.PULLUPEN), inversion (INVEN) and any external
 * pull configuration. Mirrors avr_ioport_update_irqs().
 */
static void
avr_port_modern_update_irqs(avr_port_modern_t *p)
{
	avr_t *avr = p->io.avr;
	uint8_t ddr = avr->data[p->r_dir];
	uint8_t out = avr->data[p->r_out];
	uint8_t inv = pin_mask_from_ctrl(p, PINCTRL_INVEN);
	uint8_t pull = pin_mask_from_ctrl(p, PINCTRL_PULLUPEN);

	for (int i = 0; i < 8; i++) {
		if (ddr & (1 << i)) {
			uint8_t b = (out >> i) & 1;
			if (inv & (1 << i))
				b ^= 1;
			avr_raise_irq(p->io.irq + i, b);
		} else if (!avr->options.no_pullups) {
			if (p->external.pull_mask & (1 << i))
				avr_raise_irq(p->io.irq + i,
							  (p->external.pull_value >> i) & 1);
			else if (pull & (1 << i))
				avr_raise_irq(p->io.irq + i, 1);
		}
	}

	uint8_t pin = (avr->data[p->r_in] & ~ddr) | (out & ddr);
	pin ^= inv;
	pin = (pin & ~p->external.pull_mask) | p->external.pull_value;
	avr_raise_irq(p->io.irq + IOPORT_IRQ_PIN_ALL, pin);

	/* Mirror to any IRQs hooked on the OUT register (e.g. VCD dumps). */
	avr_io_addr_t out_io = AVR_DATA_TO_IO(p->r_out);
	if (avr->io[out_io].irq && !p->irqing) {
		avr_raise_irq(avr->io[out_io].irq + AVR_IOMEM_IRQ_ALL, out);
		for (int i = 0; i < 8; i++)
			avr_raise_irq(avr->io[out_io].irq + i, (out >> i) & 1);
	}
}

/* ---- DIR group ---- */

static void
avr_port_modern_dir_write(struct avr_t *avr, avr_io_addr_t addr,
						  uint8_t v, void *param)
{
	avr_port_modern_t *p = (avr_port_modern_t *)param;
	avr_core_watch_write(avr, p->r_dir, v);
	avr_raise_irq(p->io.irq + IOPORT_IRQ_DIRECTION_ALL, v);
	avr_port_modern_update_irqs(p);
}

static void
avr_port_modern_dirset_write(struct avr_t *avr, avr_io_addr_t addr,
							 uint8_t v, void *param)
{
	avr_port_modern_t *p = (avr_port_modern_t *)param;
	avr_port_modern_dir_write(avr, p->r_dir, avr->data[p->r_dir] | v, p);
}
static void
avr_port_modern_dirclr_write(struct avr_t *avr, avr_io_addr_t addr,
							 uint8_t v, void *param)
{
	avr_port_modern_t *p = (avr_port_modern_t *)param;
	avr_port_modern_dir_write(avr, p->r_dir, avr->data[p->r_dir] & ~v, p);
}
static void
avr_port_modern_dirtgl_write(struct avr_t *avr, avr_io_addr_t addr,
							 uint8_t v, void *param)
{
	avr_port_modern_t *p = (avr_port_modern_t *)param;
	avr_port_modern_dir_write(avr, p->r_dir, avr->data[p->r_dir] ^ v, p);
}

/* ---- OUT group ---- */

static void
avr_port_modern_out_write(struct avr_t *avr, avr_io_addr_t addr,
						  uint8_t v, void *param)
{
	avr_port_modern_t *p = (avr_port_modern_t *)param;
	avr_core_watch_write(avr, p->r_out, v);
	avr_raise_irq(p->io.irq + IOPORT_IRQ_REG_PORT, v);
	avr_port_modern_update_irqs(p);
}

static void
avr_port_modern_outset_write(struct avr_t *avr, avr_io_addr_t addr,
							 uint8_t v, void *param)
{
	avr_port_modern_t *p = (avr_port_modern_t *)param;
	avr_port_modern_out_write(avr, p->r_out, avr->data[p->r_out] | v, p);
}
static void
avr_port_modern_outclr_write(struct avr_t *avr, avr_io_addr_t addr,
							 uint8_t v, void *param)
{
	avr_port_modern_t *p = (avr_port_modern_t *)param;
	avr_port_modern_out_write(avr, p->r_out, avr->data[p->r_out] & ~v, p);
}
static void
avr_port_modern_outtgl_write(struct avr_t *avr, avr_io_addr_t addr,
							 uint8_t v, void *param)
{
	avr_port_modern_t *p = (avr_port_modern_t *)param;
	avr_port_modern_out_write(avr, p->r_out, avr->data[p->r_out] ^ v, p);
}

/* SET/CLR/TGL read back the current DIR / OUT value (they are aliases). */
static uint8_t
avr_port_modern_read_dir(struct avr_t *avr, avr_io_addr_t addr, void *param)
{
	avr_port_modern_t *p = (avr_port_modern_t *)param;
	return avr->data[p->r_dir];
}
static uint8_t
avr_port_modern_read_out(struct avr_t *avr, avr_io_addr_t addr, void *param)
{
	avr_port_modern_t *p = (avr_port_modern_t *)param;
	return avr->data[p->r_out];
}

/* ---- IN ---- */

static uint8_t
avr_port_modern_in_read(struct avr_t *avr, avr_io_addr_t addr, void *param)
{
	avr_port_modern_t *p = (avr_port_modern_t *)param;
	uint8_t ddr = avr->data[p->r_dir];
	uint8_t inv = pin_mask_from_ctrl(p, PINCTRL_INVEN);
	uint8_t v = (avr->data[p->r_in] & ~ddr) | (avr->data[p->r_out] & ddr);
	v ^= inv;

	avr->data[p->r_in] = v;
	avr_raise_irq(p->io.irq + IOPORT_IRQ_REG_PIN, v);
	return avr_core_watch_read(avr, p->r_in);
}

/* Writing 1s to IN toggles the matching OUT bits (modern behaviour). */
static void
avr_port_modern_in_write(struct avr_t *avr, avr_io_addr_t addr,
						 uint8_t v, void *param)
{
	avr_port_modern_t *p = (avr_port_modern_t *)param;
	avr_port_modern_out_write(avr, p->r_out, avr->data[p->r_out] ^ v, p);
}

/* ---- INTFLAGS ---- */

static void
avr_port_modern_intflags_write(struct avr_t *avr, avr_io_addr_t addr,
							   uint8_t v, void *param)
{
	avr_port_modern_t *p = (avr_port_modern_t *)param;
	/* write-1-to-clear */
	uint8_t res = avr->data[p->r_intflags] & ~v;
	avr_core_watch_write(avr, p->r_intflags, res);
	if (!res)
		avr_clear_interrupt(avr, &p->port_vect);
}

/*
 * Pin change / edge detection. Adapted from avr_ioport_irq_notify(): handles
 * both an external driver changing an input pin and user code forcing a pin,
 * then evaluates each affected input pin's PINnCTRL.ISC to raise PORTx_PORT.
 */
void
avr_port_modern_irq_notify(struct avr_irq_t *irq, uint32_t value, void *param)
{
	avr_port_modern_t *p = (avr_port_modern_t *)param;
	avr_t *avr = p->io.avr;
	int output = value & AVR_IOPORT_OUTPUT;
	uint8_t ddr = avr->data[p->r_dir];
	uint8_t mask, new_pin;
	uint8_t old_in = avr->data[p->r_in];

	if (irq->irq == IOPORT_IRQ_PIN_ALL_IN)
		mask = 0xff;
	else
		mask = (1 << irq->irq);
	value &= 0xff;
	if (value && irq->irq != IOPORT_IRQ_PIN_ALL_IN)
		value = mask;
	new_pin = (avr->data[p->r_in] & ~mask) | (value & mask);
	new_pin = (avr->data[p->r_out] & ddr) | (new_pin & ~ddr);

	if (output) {
		uint8_t new_out = (avr->data[p->r_out] & ~mask) | value;
		if (mask & ddr) {
			avr_port_modern_out_write(avr, p->r_out, new_out, p);
			avr_core_watch_write(avr, p->r_in, new_pin);
		} else {
			avr->data[p->r_out] = new_out;
			irq->flags |= IRQ_FLAG_NTF_STOP;
			return;
		}
	} else {
		if (irq->irq == IOPORT_IRQ_PIN_ALL_IN) {
			p->irqing = 1;
			for (int i = 0; i < 8; ++i)
				avr_raise_irq(p->io.irq + i, (new_pin >> i) & 1);
			avr_raise_irq(p->io.irq + IOPORT_IRQ_PIN_ALL, new_pin);
			p->irqing = 0;
		}
		avr_core_watch_write(avr, p->r_in, new_pin);
	}

	/* Evaluate pin interrupts on the input pins that changed. */
	uint8_t raised = 0;
	for (int i = 0; i < 8; i++) {
		if (!(mask & (1 << i)) || (ddr & (1 << i)))
			continue;	/* only input pins in the affected mask */
		uint8_t oldb = (old_in >> i) & 1;
		uint8_t newb = (new_pin >> i) & 1;
		uint8_t isc = avr->data[p->r_pinctrl + i] & PINCTRL_ISC;
		int trig = 0;
		switch (isc) {
		case ISC_BOTHEDGES:	trig = (oldb != newb); break;
		case ISC_RISING:	trig = (!oldb && newb); break;
		case ISC_FALLING:	trig = (oldb && !newb); break;
		case ISC_LEVEL:		trig = (newb == 0); break;	/* low level */
		default:		trig = 0; break;
		}
		if (trig)
			raised |= (1 << i);
	}
	if (raised) {
		avr_core_watch_write(avr, p->r_intflags,
							 avr->data[p->r_intflags] | raised);
		avr_raise_interrupt(avr, &p->port_vect);
	}
}

static void
avr_port_modern_reset(avr_io_t *io)
{
	avr_port_modern_t *p = (avr_port_modern_t *)io;
	for (int i = 0; i < IOPORT_IRQ_PIN_ALL; i++)
		avr_irq_register_notify(p->io.irq + i, avr_port_modern_irq_notify, p);
	avr_irq_register_notify(p->io.irq + IOPORT_IRQ_PIN_ALL_IN,
							avr_port_modern_irq_notify, p);
}

static int
avr_port_modern_ioctl(struct avr_io_t *io, uint32_t ctl, void *io_param)
{
	avr_port_modern_t *p = (avr_port_modern_t *)io;
	avr_t *avr = p->io.avr;
	int res = -1;

	if (!io_param)
		return -1;

	if (ctl == AVR_IOCTL_IOPORT_GETIRQ_REGBIT) {
		avr_ioport_getirq_t *r = (avr_ioport_getirq_t *)io_param;
		if (r->bit.reg == p->r_out || r->bit.reg == p->r_in ||
			r->bit.reg == p->r_dir) {
			int o = 0;
			if (r->bit.mask == 0xff)
				r->irq[o++] = &p->io.irq[IOPORT_IRQ_PIN_ALL];
			else
				for (int bi = 0; bi < 8; bi++)
					if (r->bit.mask & (1 << bi))
						r->irq[o++] = &p->io.irq[r->bit.bit + bi];
			if (o < 8)
				r->irq[o] = NULL;
			return o;
		}
		return -1;
	}
	if (ctl == AVR_IOCTL_IOPORT_GETSTATE(p->name)) {
		avr_ioport_state_t state = {
			.name = p->name,
			.port = avr->data[p->r_out],
			.ddr = avr->data[p->r_dir],
			.pin = avr->data[p->r_in],
		};
		*((avr_ioport_state_t *)io_param) = state;
		res = 0;
	}
	if (ctl == AVR_IOCTL_IOPORT_SET_EXTERNAL(p->name) &&
		!avr->options.no_pullups) {
		avr_ioport_external_t *m = (avr_ioport_external_t *)io_param;
		p->external.pull_mask = m->mask;
		p->external.pull_value = m->value;
		avr_port_modern_update_irqs(p);
		res = 0;
	}
	return res;
}

static const char *irq_names[IOPORT_IRQ_COUNT] = {
	[IOPORT_IRQ_PIN0] = "=pin0", [IOPORT_IRQ_PIN1] = "=pin1",
	[IOPORT_IRQ_PIN2] = "=pin2", [IOPORT_IRQ_PIN3] = "=pin3",
	[IOPORT_IRQ_PIN4] = "=pin4", [IOPORT_IRQ_PIN5] = "=pin5",
	[IOPORT_IRQ_PIN6] = "=pin6", [IOPORT_IRQ_PIN7] = "=pin7",
	[IOPORT_IRQ_PIN_ALL] = "8>all",
	[IOPORT_IRQ_PIN_ALL_IN] = "8<all_in",
	[IOPORT_IRQ_DIRECTION_ALL] = "8>ddr",
	[IOPORT_IRQ_REG_PORT] = "8>port",
	[IOPORT_IRQ_REG_PIN] = "8>pin",
};

static avr_io_t _io = {
	.kind = "port_modern",
	.reset = avr_port_modern_reset,
	.ioctl = avr_port_modern_ioctl,
	.irq_names = irq_names,
};

void
avr_port_modern_init(
		avr_t * avr,
		avr_port_modern_t * p,
		char name,
		avr_io_addr_t base,
		avr_io_addr_t vport,
		uint8_t vector)
{
	memset(p, 0, sizeof(*p));
	p->io = _io;
	p->name = name;
	p->base = base;
	p->vport = vport;
	p->r_dir = base + PORTM_DIR;
	p->r_out = base + PORTM_OUT;
	p->r_in = base + PORTM_IN;
	p->r_intflags = base + PORTM_INTFLAGS;
	p->r_pinctrl = base + PORTM_PIN0CTRL;

	/* PORTx_PORT interrupt: requested by any INTFLAGS bit (enabled per-pin via
	 * PINnCTRL.ISC), so use INTFLAGS as the "enable" the engine checks and set
	 * the flag bits directly in the notify handler (sticky / W1C). */
	p->port_vect.vector = vector;
	p->port_vect.enable.reg = p->r_intflags;
	p->port_vect.enable.bit = 0;
	p->port_vect.enable.mask = 0xff;
	p->port_vect.raise_sticky = 1;

	avr_register_io(avr, &p->io);
	avr_register_vector(avr, &p->port_vect);
	avr_io_setirqs(&p->io, AVR_IOCTL_IOPORT_GETIRQ(p->name), IOPORT_IRQ_COUNT, NULL);

	for (int i = 0; i < IOPORT_IRQ_REG_PIN; i++) {
		p->io.irq[i].flags |= IRQ_FLAG_FILTERED;
		if (i < IOPORT_IRQ_PIN_ALL)
			p->io.irq[i].flags &= ~IRQ_FLAG_INIT;
	}

	/* DIR + aliases */
	avr_register_io_write(avr, p->r_dir, avr_port_modern_dir_write, p);
	avr_register_io_write(avr, base + PORTM_DIRSET, avr_port_modern_dirset_write, p);
	avr_register_io_write(avr, base + PORTM_DIRCLR, avr_port_modern_dirclr_write, p);
	avr_register_io_write(avr, base + PORTM_DIRTGL, avr_port_modern_dirtgl_write, p);
	avr_register_io_read(avr, base + PORTM_DIRSET, avr_port_modern_read_dir, p);
	avr_register_io_read(avr, base + PORTM_DIRCLR, avr_port_modern_read_dir, p);
	avr_register_io_read(avr, base + PORTM_DIRTGL, avr_port_modern_read_dir, p);

	/* OUT + aliases */
	avr_register_io_write(avr, p->r_out, avr_port_modern_out_write, p);
	avr_register_io_write(avr, base + PORTM_OUTSET, avr_port_modern_outset_write, p);
	avr_register_io_write(avr, base + PORTM_OUTCLR, avr_port_modern_outclr_write, p);
	avr_register_io_write(avr, base + PORTM_OUTTGL, avr_port_modern_outtgl_write, p);
	avr_register_io_read(avr, base + PORTM_OUTSET, avr_port_modern_read_out, p);
	avr_register_io_read(avr, base + PORTM_OUTCLR, avr_port_modern_read_out, p);
	avr_register_io_read(avr, base + PORTM_OUTTGL, avr_port_modern_read_out, p);

	/* IN + INTFLAGS */
	avr_register_io_read(avr, p->r_in, avr_port_modern_in_read, p);
	avr_register_io_write(avr, p->r_in, avr_port_modern_in_write, p);
	avr_register_io_write(avr, p->r_intflags, avr_port_modern_intflags_write, p);

	/* Wire the VPORT alias into the engine's low-I/O redirect table. */
	if (vport != AVR_PORT_MODERN_NO_VPORT) {
		avr->lowio_redirect[vport + VPORTM_DIR] = p->r_dir;
		avr->lowio_redirect[vport + VPORTM_OUT] = p->r_out;
		avr->lowio_redirect[vport + VPORTM_IN] = p->r_in;
		avr->lowio_redirect[vport + VPORTM_INTFLAGS] = p->r_intflags;
	}
}
