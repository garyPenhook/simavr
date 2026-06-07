/*
	runner.c

	Generic modern-AVR coverage runner. Given one or more firmware .axf files on
	the command line, it loads each, instantiates the core named in the firmware's
	.mmcu section, runs it for a bounded slice of simulated time, and checks that
	PA0 actually toggled (i.e. the core booted and executed the blink loop). It
	prints a PASS/FAIL line per device and exits non-zero if any failed.

	Used by this board's `make run` target to smoke-test every supported modern
	core (all 15 tinyAVR 1-series + 8 megaAVR 0-series) with one binary.

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
#include <string.h>

#include "sim_avr.h"
#include "avr_ioport.h"
#include "avr_uart.h"
#include "sim_elf.h"

static unsigned long g_toggles;
static unsigned long g_tx_bytes;

static void
pa0_changed_hook(struct avr_irq_t * irq, uint32_t value, void * param)
{
	g_toggles++;
}

static void
usart0_tx_hook(struct avr_irq_t * irq, uint32_t value, void * param)
{
	g_tx_bytes++;
}

/* Returns 0 on PASS, 1 on FAIL. */
static int
run_one(const char * fname)
{
	elf_firmware_t f = {{0}};

	if (elf_read_firmware(fname, &f) < 0) {
		printf("FAIL  %-28s (cannot read firmware)\n", fname);
		return 1;
	}

	avr_t * avr = avr_make_mcu_by_name(f.mmcu);
	if (!avr) {
		printf("FAIL  %-12s %-15s (core not registered)\n", f.mmcu, fname);
		return 1;
	}
	avr_init(avr);
	avr_load_firmware(avr, &f);

	g_toggles = 0;
	g_tx_bytes = 0;
	avr_irq_register_notify(
		avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ('A'), IOPORT_IRQ_PIN0),
		pa0_changed_hook, NULL);
	avr_irq_register_notify(
		avr_io_getirq(avr, AVR_IOCTL_UART_GETIRQ('0'), UART_IRQ_OUTPUT),
		usart0_tx_hook, NULL);

	// run ~100 ms of simulated time, bounded so a wedged core can't hang us
	uint64_t stop_cycle = avr->frequency / 10;
	int state = cpu_Running;
	while (state != cpu_Done && state != cpu_Crashed && avr->cycle < stop_cycle)
		state = avr_run(avr);

	int crashed = (state == cpu_Crashed);
	int ok = !crashed && g_toggles > 0 && g_tx_bytes > 0;
	printf("%s  %-12s %-15s PA0x%lu USART0-TXx%lu%s\n",
		ok ? "PASS" : "FAIL", f.mmcu, fname, g_toggles, g_tx_bytes,
		crashed ? " (CPU crashed)" : "");

	avr_terminate(avr);
	return ok ? 0 : 1;
}

int
main(int argc, char *argv[])
{
	if (argc < 2) {
		fprintf(stderr,
			"usage: %s <firmware.axf> [more.axf ...]\n"
			"  runs each modern-AVR firmware and checks PA0 toggles\n",
			argv[0]);
		return 2;
	}

	int failures = 0, total = argc - 1;
	for (int i = 1; i < argc; i++)
		failures += run_one(argv[i]);

	printf("\n%d/%d cores PASSED\n", total - failures, total);
	return failures ? 1 : 0;
}
