/*
	runner.c

	Generic modern-AVR coverage runner. Given one or more firmware .axf files on
	the command line, it loads each, instantiates the core named in the firmware's
	.mmcu section, runs it for a bounded slice of simulated time, and decodes the
	per-peripheral result bitmask the firmware (blink_fw.c) streams over USART0.
	A core PASSes only if every required peripheral responded.

	The peripherals exercised: PORT, TCA0, TCB0, RTC, ADC0, SPI0 and TWI0 — all
	present identically on every modern part. TWI needs an external responder, so
	this host attaches a tiny I2C slave at 0x50 that ACKs the firmware's address.

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
#include "avr_twi.h"
#include "sim_elf.h"

/* Result bits, one per peripheral. Kept in sync with blink_fw.c. */
#define R_PORT	(1 << 0)
#define R_TCA	(1 << 1)
#define R_TCB	(1 << 2)
#define R_RTC	(1 << 3)
#define R_ADC	(1 << 4)
#define R_SPI	(1 << 5)
#define R_TWI	(1 << 6)
#define R_ALL	(R_PORT | R_TCA | R_TCB | R_RTC | R_ADC | R_SPI | R_TWI)

static const struct { uint8_t bit; const char *name; } k_periphs[] = {
	{ R_PORT, "PORT" }, { R_TCA, "TCA0" }, { R_TCB, "TCB0" }, { R_RTC, "RTC" },
	{ R_ADC, "ADC0" }, { R_SPI, "SPI0" }, { R_TWI, "TWI0" },
};

static uint8_t  g_result;	/* OR of every result byte the firmware sent */
static avr_irq_t *g_twi_reply;	/* raise slave ACKs/data on this IRQ */

static void
usart0_rx_hook(struct avr_irq_t * irq, uint32_t value, void * param)
{
	g_result |= (uint8_t)value;
}

/* Minimal I2C slave at 0x50: ACK our address and any byte written to it. */
static void
twi_slave_hook(struct avr_irq_t * irq, uint32_t value, void * param)
{
	avr_twi_msg_irq_t m;
	m.u.v = value;

	if ((m.u.twi.msg & TWI_COND_START) && (m.u.twi.addr >> 1) == 0x50)
		avr_raise_irq(g_twi_reply, avr_twi_irq_msg(TWI_COND_ACK, m.u.twi.addr, 1));
	else if (m.u.twi.msg & TWI_COND_WRITE)
		avr_raise_irq(g_twi_reply, avr_twi_irq_msg(TWI_COND_ACK, m.u.twi.addr, 1));
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

	g_result = 0;

	// collect the per-peripheral result bytes the firmware streams on USART0
	avr_irq_register_notify(
		avr_io_getirq(avr, AVR_IOCTL_UART_GETIRQ('0'), UART_IRQ_OUTPUT),
		usart0_rx_hook, NULL);

	// attach the I2C slave responder so TWI0 master transactions complete
	g_twi_reply = avr_io_getirq(avr, AVR_IOCTL_TWI_GETIRQ('0'), TWI_IRQ_INPUT);
	avr_irq_register_notify(
		avr_io_getirq(avr, AVR_IOCTL_TWI_GETIRQ('0'), TWI_IRQ_OUTPUT),
		twi_slave_hook, NULL);

	// run ~200 ms of simulated time, bounded so a wedged core can't hang us
	// (the 32 kHz RTC needs a few ms of real time before its count is visible)
	uint64_t stop_cycle = avr->frequency / 5;
	int state = cpu_Running;
	while (state != cpu_Done && state != cpu_Crashed && avr->cycle < stop_cycle)
		state = avr_run(avr);

	int crashed = (state == cpu_Crashed);
	int ok = !crashed && (g_result & R_ALL) == R_ALL;

	printf("%s  %-12s ", ok ? "PASS" : "FAIL", f.mmcu);
	for (size_t i = 0; i < sizeof(k_periphs) / sizeof(k_periphs[0]); i++)
		printf("%s%c ", k_periphs[i].name, (g_result & k_periphs[i].bit) ? '+' : '-');
	if (crashed)
		printf("(CPU crashed)");
	printf("\n");

	avr_terminate(avr);
	return ok ? 0 : 1;
}

int
main(int argc, char *argv[])
{
	if (argc < 2) {
		fprintf(stderr,
			"usage: %s <firmware.axf> [more.axf ...]\n"
			"  runs each modern-AVR firmware and checks its peripherals respond\n"
			"  (PORT TCA0 TCB0 RTC ADC0 SPI0 TWI0); '+' = responded, '-' = not\n",
			argv[0]);
		return 2;
	}

	int failures = 0, total = argc - 1;
	for (int i = 1; i < argc; i++)
		failures += run_one(argv[i]);

	printf("\n%d/%d cores PASSED\n", total - failures, total);
	return failures ? 1 : 0;
}
