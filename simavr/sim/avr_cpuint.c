/*
	avr_cpuint.c

	Modern AVR CPUINT register block. See avr_cpuint.h.

	Copyright 2024 simavr modern-AVR fork (garyPenhook)

	This file is part of simavr. GNU GPL v3 or later; see COPYING.
 */

#include "avr_cpuint.h"
#include "sim_interrupts.h"

static void
avr_cpuint_ctrla_write(avr_t * avr, avr_io_addr_t addr, uint8_t v, void * param)
{
	avr->data[addr] = v;
	// Only the round-robin bit affects scheduling here; CVT/IVSEL (vector
	// table relocation / compaction) are not modelled.
	avr_cpuint_set_lvl0rr(avr, v & AVR_CPUINT_CTRLA_LVL0RR);
}

static void
avr_cpuint_lvl0pri_write(avr_t * avr, avr_io_addr_t addr, uint8_t v, void * param)
{
	avr->data[addr] = v;
	avr_cpuint_set_lvl0pri(avr, v);
}

static uint8_t
avr_cpuint_lvl0pri_read(avr_t * avr, avr_io_addr_t addr, void * param)
{
	// In round-robin mode the engine updates LVL0PRI as interrupts are
	// acknowledged; reflect the live value.
	return avr->interrupts.cpuint_lvl0pri;
}

static void
avr_cpuint_lvl1vec_write(avr_t * avr, avr_io_addr_t addr, uint8_t v, void * param)
{
	avr->data[addr] = v;
	avr_cpuint_set_lvl1vec(avr, v);
}

static uint8_t
avr_cpuint_status_read(avr_t * avr, avr_io_addr_t addr, void * param)
{
	return avr_cpuint_get_status(avr);
}

void
avr_cpuint_init(avr_t * avr, avr_cpuint_t * p)
{
	p->io.kind = "cpuint";
	avr_register_io(avr, &p->io);

	avr_register_io_write(avr, p->r_base + AVR_CPUINT_R_CTRLA,
			avr_cpuint_ctrla_write, p);
	avr_register_io_read(avr, p->r_base + AVR_CPUINT_R_STATUS,
			avr_cpuint_status_read, p);
	avr_register_io_write(avr, p->r_base + AVR_CPUINT_R_LVL0PRI,
			avr_cpuint_lvl0pri_write, p);
	avr_register_io_read(avr, p->r_base + AVR_CPUINT_R_LVL0PRI,
			avr_cpuint_lvl0pri_read, p);
	avr_register_io_write(avr, p->r_base + AVR_CPUINT_R_LVL1VEC,
			avr_cpuint_lvl1vec_write, p);
}
