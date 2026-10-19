/*
	test_attiny3224_selftest.c

	Host-side validation for the tinyAVR 2-series cores: it loads the companion
	firmware (attiny3224_selftest.axf, real C compiled with avr-gcc), runs it on
	simavr, and checks the pass bitmasks the firmware leaves in its SRAM result
	block (OUT[0]: analog, EVSYS, CCL, NVM subtests; OUT[1]: interrupt
	vectors) once OUT[2] reads the 0xA5 done sentinel.

	This complements test_avrxt_engine.c: that drives the peripheral models from
	the host with hand-assembled opcodes, while this runs genuine compiled
	firmware that pokes the registers via the device header, so it also catches
	register-layout mismatches between the model and what real firmware uses.

	Built and run via `make attiny3224-selftest` (needs a modern avr-gcc with
	ATtiny3224 support). Exits 0 on success, non-zero otherwise.

	Copyright 2026 simavr authors. GNU GPL v3 or later; see COPYING.
 */

#include <stdio.h>
#include <string.h>
#include "sim_avr.h"
#include "sim_elf.h"

/* Firmware result block (data space), see attiny3224_selftest.c. GPIOR0-3 are not
 * usable here: simavr keeps r0-r31 in data[0..31], aliasing them. */
#define RESULT0_ADDR	0x3f00
#define RESULT1_ADDR	0x3f01
#define DONE_ADDR	0x3f02
#define DONE_SENTINEL	0xa5

/* Subtest bits, mirroring attiny3224_selftest.c. */
static const struct { uint16_t reg; uint8_t bit; const char *name; } subtests[] = {
	{ RESULT0_ADDR, 1 << 0, "EEPROM write/read (NVMCTRL)" },
	{ RESULT0_ADDR, 1 << 1, "temperature sensor (SIGROW cal)" },
	{ RESULT0_ADDR, 1 << 2, "ADC 12-bit: VDD/10 = ~409" },
	{ RESULT0_ADDR, 1 << 3, "ADC burst accumulation + scaling (AC0 DACREF)" },
	{ RESULT0_ADDR, 1 << 4, "ADC differential (VDD/10 - DACREF) < 0" },
	{ RESULT0_ADDR, 1 << 5, "ADC window comparator" },
	{ RESULT0_ADDR, 1 << 6, "EVSYS software event -> ADC start" },
	{ RESULT0_ADDR, 1 << 7, "CCL LUT0 rising-edge flag" },
	{ RESULT1_ADDR, 1 << 0, "ADC0_RESRDY interrupt vector" },
	{ RESULT1_ADDR, 1 << 1, "CCL_CCL interrupt vector" },
	{ RESULT1_ADDR, 1 << 2, "ADC0_ERROR interrupt vector (trigger overrun)" },
};
#define N_SUBTESTS	((int)(sizeof(subtests) / sizeof(subtests[0])))

int
main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "attiny3224_selftest.axf";
	elf_firmware_t fw;
	memset(&fw, 0, sizeof(fw));
	if (elf_read_firmware(path, &fw)) {
		fprintf(stderr, "could not read firmware '%s'\n", path);
		return 2;
	}

	const char *mmcu = fw.mmcu[0] ? fw.mmcu : "attiny3224";
	avr_t *avr = avr_make_mcu_by_name(mmcu);
	if (!avr) { fprintf(stderr, "no core for '%s'\n", mmcu); return 2; }
	avr->log = LOG_ERROR;
	avr_init(avr);
	avr_load_firmware(avr, &fw);
	if (!avr->frequency)
		avr->frequency = 8000000;

	long i;
	for (i = 0; i < 100000000 && avr->data[DONE_ADDR] != DONE_SENTINEL &&
		 avr->state != cpu_Crashed && avr->state != cpu_Done; i++)
		avr_run(avr);

	if (avr->data[DONE_ADDR] != DONE_SENTINEL) {
		printf("FAIL: firmware did not finish (no done sentinel) after %ld cycles\n",
			   (long)avr->cycle);
		return 1;
	}

	printf("attiny3224 self-test on %s @ %u Hz (%llu cycles):\n",
		   mmcu, avr->frequency, (unsigned long long)avr->cycle);
	int bad = 0;
	for (int s = 0; s < N_SUBTESTS; s++) {
		int ok = (avr->data[subtests[s].reg] & subtests[s].bit) != 0;
		printf("  %-4s %s\n", ok ? "ok:" : "FAIL", subtests[s].name);
		bad += !ok;
	}

	if (bad) {
		printf("FAILED (%d of %d subtests; OUT[0]=0x%02x OUT[1]=0x%02x)\n", bad,
			   N_SUBTESTS, avr->data[RESULT0_ADDR], avr->data[RESULT1_ADDR]);
		return 1;
	}
	printf("PASSED (%d subtests)\n", N_SUBTESTS);
	return 0;
}
