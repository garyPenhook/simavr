/*
	test_attiny3217_selftest.c

	Host-side validation for the ATtiny3217 modern-AVR core: it loads the
	companion firmware (attiny3217_selftest.axf, real C compiled with avr-gcc),
	runs it on simavr, and checks the self-test result the firmware leaves in
	GPIOR0 (a per-subtest pass bitmask) once GPIOR1 reads the 0xA5 done sentinel.

	This complements test_avrxt_engine.c: that drives the peripheral models from
	the host with hand-assembled opcodes, while this runs genuine compiled
	firmware that pokes the registers via the device headers — so it also catches
	register-layout mismatches between the model and what real firmware uses.

	Built and run via `make attiny3217-selftest` (needs a modern avr-gcc with
	ATtiny3217 support). Exits 0 on success, non-zero otherwise.

	Copyright 2026 simavr authors. GNU GPL v3 or later; see COPYING.
 */

#include <stdio.h>
#include <string.h>
#include "sim_avr.h"
#include "sim_elf.h"

/* GPIO general-purpose registers (data space). */
#define GPIOR0_ADDR	0x1c
#define GPIOR1_ADDR	0x1d
#define DONE_SENTINEL	0xa5

/* Subtest bits, mirroring attiny3217_selftest.c. */
static const struct { uint8_t bit; const char *name; } subtests[] = {
	{ 1 << 0, "EEPROM write/read (NVMCTRL)" },
	{ 1 << 1, "temperature sensor (SIGROW cal)" },
	{ 1 << 2, "TCB0 periodic counting" },
	{ 1 << 3, "TCA0 overflow counting" },
};
#define N_SUBTESTS	((int)(sizeof(subtests) / sizeof(subtests[0])))
#define EXPECTED_MASK	0x0f

int
main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "attiny3217_selftest.axf";
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
		avr->frequency = 8000000;

	long i;
	for (i = 0; i < 50000000 && avr->data[GPIOR1_ADDR] != DONE_SENTINEL &&
		 avr->state != cpu_Crashed && avr->state != cpu_Done; i++)
		avr_run(avr);

	if (avr->data[GPIOR1_ADDR] != DONE_SENTINEL) {
		printf("FAIL: firmware did not finish (no done sentinel) after %ld cycles\n",
			   (long)avr->cycle);
		return 1;
	}

	uint8_t result = avr->data[GPIOR0_ADDR];
	printf("attiny3217 self-test on %s @ %u Hz (%llu cycles):\n",
		   mmcu, avr->frequency, (unsigned long long)avr->cycle);
	for (int s = 0; s < N_SUBTESTS; s++) {
		int ok = (result & subtests[s].bit) != 0;
		printf("  %-4s %s\n", ok ? "ok:" : "FAIL", subtests[s].name);
	}

	if (result != EXPECTED_MASK) {
		printf("FAILED (result 0x%02x, expected 0x%02x)\n", result, EXPECTED_MASK);
		return 1;
	}
	printf("PASSED (%d subtests)\n", N_SUBTESTS);
	return 0;
}
