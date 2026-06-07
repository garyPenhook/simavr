/*
	blink.c

	Host-side "board" for the attiny3217 modern-AVR blink demo, the sibling of
	examples/board_atmega4809. It loads the firmware, watches PA0 toggle through
	the modern PORT model (which reuses the classic IOPORT IRQ mesh, so the hook
	below is identical to a classic board), records PA0 to a VCD file, and runs
	for a bounded number of simulated milliseconds so the demo terminates on its
	own.

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

#include <stdlib.h>
#include <stdio.h>

#include "sim_avr.h"
#include "avr_ioport.h"
#include "sim_elf.h"
#include "sim_vcd_file.h"

avr_t * avr = NULL;
avr_vcd_t vcd_file;

static unsigned long pa0_toggles = 0;

/*
 * Called whenever PA0 changes. The modern PORT model raises the same
 * IOPORT_IRQ_PINx IRQs as the classic ioport, so nothing here is modern-aware.
 */
static void
pa0_changed_hook(struct avr_irq_t * irq, uint32_t value, void * param)
{
	pa0_toggles++;
	if (pa0_toggles <= 6)
		printf("PA0 -> %u\n", value);
}

int
main(int argc, char *argv[])
{
	elf_firmware_t f = {{0}};
	const char * fname = "attiny3217_blink.axf";

	printf("Firmware pathname is %s\n", fname);
	if (elf_read_firmware(fname, &f) < 0) {
		fprintf(stderr, "%s: unable to load firmware %s\n", argv[0], fname);
		exit(1);
	}
	printf("firmware %s f=%d mmcu=%s\n", fname, (int)f.frequency, f.mmcu);

	avr = avr_make_mcu_by_name(f.mmcu);
	if (!avr) {
		fprintf(stderr, "%s: AVR '%s' not known\n", argv[0], f.mmcu);
		exit(1);
	}
	avr_init(avr);
	avr_load_firmware(avr, &f);

	// watch PA0 toggle (modern PORT reuses the classic IOPORT IRQ mesh)
	avr_irq_register_notify(
		avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ('A'), IOPORT_IRQ_PIN0),
		pa0_changed_hook, NULL);

	// trace PA0 to a VCD file (open in GTKWave)
	avr_vcd_init(avr, "attiny3217_blink.vcd", &vcd_file, 1000 /* usec */);
	avr_vcd_add_signal(&vcd_file,
		avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ('A'), IOPORT_IRQ_PIN0),
		1 /* bit */, "PA0");
	avr_vcd_start(&vcd_file);

	printf("\nDemo launching: running for 200 simulated ms...\n");

	// bound the run so the demo exits on its own (≈200 ms of sim time)
	uint64_t stop_cycle = avr->frequency / 5;
	int state = cpu_Running;
	while (state != cpu_Done && state != cpu_Crashed && avr->cycle < stop_cycle)
		state = avr_run(avr);

	avr_vcd_stop(&vcd_file);
	printf("\nDone: PA0 toggled %lu times in 200 ms; trace in attiny3217_blink.vcd\n",
		pa0_toggles);
	return 0;
}
