/*
	test_attiny3217_blink.c

	Host-side demo/regression for the ATtiny3217 modern-AVR core: it loads the
	companion firmware ELF (attiny3217_blink.axf, built from attiny3217_blink.c),
	runs it on simavr, and checks that the LED pin PA0 actually toggles. Exits 0
	on success, non-zero otherwise (so it doubles as a test).

	This is built and run via `make attiny3217-demo` rather than the default test
	harness, because building the firmware needs a modern avr-gcc with ATtiny3217
	support (stock packaged toolchains do not have it).

	Copyright 2026 simavr authors. GNU GPL v3 or later; see COPYING.
 */

#include <stdio.h>
#include <string.h>
#include "sim_avr.h"
#include "sim_elf.h"
#include "sim_irq.h"
#include "avr_ioport.h"

static int toggles;
static uint8_t last = 0xff;

static void on_pa0(struct avr_irq_t *irq, uint32_t v, void *param)
{
	(void)irq; (void)param;
	if (last != 0xff && (v & 1) != last)
		toggles++;
	last = v & 1;
}

int
main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "attiny3217_blink.axf";
	elf_firmware_t fw;
	memset(&fw, 0, sizeof(fw));
	if (elf_read_firmware(path, &fw)) {
		fprintf(stderr, "could not read firmware '%s'\n", path);
		return 2;
	}

	const char *mmcu = fw.mmcu[0] ? fw.mmcu : "attiny3217";
	avr_t *avr = avr_make_mcu_by_name(mmcu);
	if (!avr) { fprintf(stderr, "no core for '%s'\n", mmcu); return 2; }
	avr->log = LOG_ERROR;
	avr_init(avr);
	avr_load_firmware(avr, &fw);
	if (!avr->frequency)
		avr->frequency = 3333333;

	avr_irq_register_notify(
		avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ('A'), IOPORT_IRQ_PIN0),
		on_pa0, NULL);

	for (long i = 0; i < 5000000 && toggles < 10 &&
		 avr->state != cpu_Crashed && avr->state != cpu_Done; i++)
		avr_run(avr);

	printf("attiny3217 blink: %s loaded on %s @ %u Hz -> PA0 toggled %d times "
		   "in %llu cycles\n",
		   path, mmcu, avr->frequency, toggles,
		   (unsigned long long)avr->cycle);

	if (toggles < 2) {
		printf("FAIL: expected the LED on PA0 to toggle\n");
		return 1;
	}
	printf("PASS\n");
	return 0;
}
