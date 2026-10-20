/*
	avr_extint.c

	Copyright 2008, 2009 Michel Pollet <buserror@gmail.com>

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
#include <stdlib.h>
#include <string.h>
#include "avr_extint.h"
#include "avr_ioport.h"

typedef struct avr_extint_poll_context_t {
	uint32_t	eint_no; // index of particular interrupt source we are monitoring
	avr_extint_t *extint;
} avr_extint_poll_context_t;

/*
 * Level-triggered INTn: the interrupt flag is always read as cleared (ATtiny13A DS40002307A, GIFR
 * INTF0: "This flag is always cleared when INT0 is configured as a level interrupt"; the same
 * sentence is in the megaAVR data sheets). The request lives in vector.pending, so drop the flag
 * avr_raise_interrupt() just set.
 */
static void avr_extint_raise_level(avr_t * avr, avr_extint_t * p, int no)
{
	avr_raise_interrupt(avr, &p->eint[no].vector);
	if (p->eint[no].vector.raised.reg)
		avr_regbit_clear(avr, p->eint[no].vector.raised);
}

static avr_cycle_count_t avr_extint_poll_level_trig(
		struct avr_t * avr,
		avr_cycle_count_t when,
		void * param)
{
	avr_extint_poll_context_t *poll = (avr_extint_poll_context_t *)param;
	avr_extint_t * p = poll->extint;

	/* Check for change of interrupt mode. */

	if (avr_regbit_get_array(avr, p->eint[poll->eint_no].isc, 2))
		goto terminate_poll;

	uint8_t port = p->eint[poll->eint_no].port_ioctl & 0xFF;
	avr_ioport_state_t iostate;
	if (avr_ioctl(avr, AVR_IOCTL_IOPORT_GETSTATE( port ), &iostate) < 0)
		goto terminate_poll;
	uint8_t bit = ( iostate.pin >> p->eint[poll->eint_no].port_pin ) & 1;
	if (bit)
		goto terminate_poll; // Only poll while pin level remains low

	if (avr->sreg[S_I]) {
		uint8_t raised = avr_regbit_get(avr, p->eint[poll->eint_no].vector.raised) || p->eint[poll->eint_no].vector.pending;
		if (!raised)
			avr_extint_raise_level(avr, p, poll->eint_no);
	}

	return when+1;

terminate_poll:
	p->eint[poll->eint_no].poll_live = 0;
	free(poll);
	return 0;
}

static avr_extint_t * avr_extint_get(avr_t * avr)
{
	if (!avr)
		return NULL;
	avr_io_t * periferal = avr->io_port;
	while (periferal) {
		if (!strcmp(periferal->kind, "extint")) {
			return (avr_extint_t *)periferal;
		}
		periferal = periferal->next;
	}
	return NULL;
}

static inline uint8_t avr_extint_exists(avr_extint_t *extint, int8_t extint_no)
{
	return (extint_no < EXTINT_COUNT) && (extint->eint[extint_no].port_ioctl);
}

/**
 * @brief avr_extint_is_strict_lvl_trig
 * @param avr
 * @param extint_no: an ext interrupt number, e.g. 0 or 1 (corresponds to INT0 or INT1)
 * @return -1 if irrelevant extint_no given, strict
 * level triggering flag otherwise.
 */
int avr_extint_is_strict_lvl_trig(avr_t * avr, uint8_t extint_no)
{
	avr_extint_t *p = avr_extint_get(avr);
	if (!p || !avr_extint_exists(p, extint_no))
		return -1;
	if (!p->eint[extint_no].isc[1].reg)
		return -1; // this is edge-only triggered interrupt
	return p->eint[extint_no].strict_lvl_trig;
}

/**
 * @brief avr_extint_set_strict_lvl_trig
 * @param avr
 * @param extint_no: an ext interrupt number, e.g. 0 or 1 (corresponds to INT0 or INT1)
 * @param strict: new value for level triggering flag
 */
void avr_extint_set_strict_lvl_trig(avr_t * avr, uint8_t extint_no, uint8_t strict)
{
	avr_extint_t *p = avr_extint_get(avr);
	if (!p || !avr_extint_exists(p, extint_no))
		return;
	if (!p->eint[extint_no].isc[1].reg)
		return; // this is edge-only triggered interrupt
	p->eint[extint_no].strict_lvl_trig = strict;
}

/*
 * A low level that is already there when INTn gets enabled, or when ISCn is switched to
 * "low level", requests the interrupt too: level sensing looks at the pin, not at an edge.
 */
static void avr_extint_start_poll(avr_t * avr, avr_extint_t * p, int no);

static void avr_extint_level_recheck(avr_t * avr, avr_extint_t * p, int no)
{
	if (!p->eint[no].port_ioctl || !p->eint[no].isc[1].reg || !p->eint[no].vector.enable.reg)
		return;
	if (avr_regbit_get_array(avr, p->eint[no].isc, 2))
		return;
	// level mode: a flag latched while the pin was in an edge mode is dropped too
	if (p->eint[no].vector.raised.reg)
		avr_regbit_clear(avr, p->eint[no].vector.raised);
	if (!avr_regbit_get(avr, p->eint[no].vector.enable))
		return;
	avr_ioport_state_t iostate;
	if (avr_ioctl(avr, AVR_IOCTL_IOPORT_GETSTATE(p->eint[no].port_ioctl & 0xFF), &iostate) < 0 ||
			(iostate.pin >> p->eint[no].port_pin) & 1 || p->eint[no].vector.pending)
		return;
	avr_extint_raise_level(avr, p, no);
	if (p->eint[no].vector.pending)
		avr_extint_start_poll(avr, p, no);
}

static void avr_extint_level_write(avr_t * avr, avr_io_addr_t addr, uint8_t v, void * param)
{
	avr_extint_t * p = (avr_extint_t *)param;

	avr_core_watch_write(avr, addr, v);
	for (int i = 0; i < EXTINT_COUNT; i++)
		avr_extint_level_recheck(avr, p, i);
}

static void avr_extint_start_poll(avr_t * avr, avr_extint_t * p, int no)
{
	if (p->eint[no].poll_live)
		return;
	avr_extint_poll_context_t *poll = malloc(sizeof(*poll));
	if (poll) {
		poll->eint_no = no;
		poll->extint = p;
		p->eint[no].poll_live = 1;
		avr_cycle_timer_register(avr, 1, avr_extint_poll_level_trig, poll);
	}
}

static void avr_extint_irq_notify(struct avr_irq_t * irq, uint32_t value, void * param)
{
	avr_extint_t * p = (avr_extint_t *)param;
	avr_t * avr = p->io.avr;

	int up = !irq->value && value;
	int down = irq->value && !value;

	// ?? uint8_t isc_bits = p->eint[irq->irq + 1].isc->reg ? 2 : 1;
	uint8_t isc_bits = p->eint[irq->irq].isc[1].reg ? 2 : 1;
	uint8_t mode = avr_regbit_get_array(avr, p->eint[irq->irq].isc, isc_bits);

	// Asynchronous interrupts, eg int2 in m16, m32 etc. support only down/up
	if (isc_bits == 1)
		mode +=2;

	switch (mode) {
		case 0: // Level triggered (low level) interrupt
			{
				/**
				  Datasheet excerpt:
					> When the external interrupt is enabled and is configured
					> as level triggered (only INT0/INT1), the interrupt
					> will trigger as long as the pin is held low.
					Thus we have to query the pin value continuously while
					it's held low and try to trigger the interrupt.
					This can be expensive, so avr_extint_set_strict_lvl_trig()
					function provisioned to allow the user to turn
					this feature off. In this case behaviour will be similar
					to the falling edge interrupt.
				 */
				if (!value) {
					uint8_t raised;

					raised = (avr_regbit_get(
								 avr,
								 p->eint[irq->irq].vector.raised) ||
							  p->eint[irq->irq].vector.pending);
					if (!raised)
						avr_extint_raise_level(avr, p, irq->irq);
					if (!raised && p->eint[irq->irq].vector.pending) {
						/* Interrupt pending, so start fast timer to re-enable
						 * it if the pin level does not change in the handler.
						 */

						avr_extint_start_poll(avr, p, irq->irq);
					}
				} else {
					avr_clear_interrupt(avr, &p->eint[irq->irq].vector);
					// If running, timer will clear on next call.
				}
			}
			break;
		case 1: // Toggle-triggered interrupt
			if (up || down)
				avr_raise_interrupt(avr, &p->eint[irq->irq].vector);
			break;
		case 2: // Falling edge triggered
			if (down)
				avr_raise_interrupt(avr, &p->eint[irq->irq].vector);
			break;
		case 3: // Rising edge trigggerd
			if (up)
				avr_raise_interrupt(avr, &p->eint[irq->irq].vector);
			break;
	}
}

static void avr_extint_reset(avr_io_t * port)
{
	avr_extint_t * p = (avr_extint_t *)port;

	for (int i = 0; i < EXTINT_COUNT; i++) {
		p->eint[i].poll_live = 0;	// avr_reset() dropped any cycle timer
		if (p->eint[i].port_ioctl) {
			avr_irq_register_notify(p->io.irq + i, avr_extint_irq_notify, p);

			if (p->eint[i].isc[1].reg) // level triggering available
				p->eint[i].strict_lvl_trig = 1; // turn on repetitive level triggering by default
			avr_irq_t * irq = avr_io_getirq(p->io.avr,
					p->eint[i].port_ioctl, p->eint[i].port_pin);

			avr_connect_irq(irq, p->io.irq + i);
		}
	}
}

static const char * irq_names[EXTINT_COUNT] = {
	[EXTINT_IRQ_OUT_INT0] = "<int0",
	[EXTINT_IRQ_OUT_INT1] = "<int1",
	[EXTINT_IRQ_OUT_INT2] = "<int2",
	[EXTINT_IRQ_OUT_INT3] = "<int3",
	[EXTINT_IRQ_OUT_INT4] = "<int4",
	[EXTINT_IRQ_OUT_INT5] = "<int5",
	[EXTINT_IRQ_OUT_INT6] = "<int6",
	[EXTINT_IRQ_OUT_INT7] = "<int7",
};

static	avr_io_t	_io = {
	.kind = "extint",
	.reset = avr_extint_reset,
	.irq_names = irq_names,
};

void avr_extint_init(avr_t * avr, avr_extint_t * p)
{
	p->io = _io;

	avr_register_io(avr, &p->io);
	for (int i = 0; i < EXTINT_COUNT; i++) {
		if (!p->eint[i].port_ioctl)
			break;
		avr_register_vector(avr, &p->eint[i].vector);
		if (p->eint[i].isc[1].reg && p->eint[i].vector.enable.reg) {	// level sensing available
			avr_register_io_write(avr, p->eint[i].vector.enable.reg, avr_extint_level_write, p);
			avr_register_io_write(avr, p->eint[i].isc[0].reg, avr_extint_level_write, p);
		}
	}
	// allocate this module's IRQ

	avr_io_setirqs(&p->io, AVR_IOCTL_EXTINT_GETIRQ(), EXTINT_COUNT, NULL);
}

