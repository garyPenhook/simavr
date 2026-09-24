/*
	csmith_runner.c

	Minimal libsimavr host used by the Csmith differential fuzzer (see
	tests/fuzz/README.md). Loads one ELF on a named core, runs it until the
	end-of-program marker planted by tests/fuzz/runtime/csmith.h, and prints
	the program's CRC32 on stdout as a single status line:

	  OK crc=XXXXXXXX cycles=N stack=M
	  TIMEOUT cycles=N
	  CRASH state=S pc=0xPPPP cycles=N
	  STACKOVF heap_start=0xHHHH lowest=0xLLLL

	The end marker is the exact sequence  BREAK ; RJMP .-2  (0x9598, 0xCFFF),
	executed with interrupts off and the CRC in X (low word) and Z (high word).
	BREAK is a no-op without a GDB session, so the core parks on the RJMP.

	Stack overflow is detected by painting the free RAM between __heap_start and
	RAMEND before the run and checking a guard band above __heap_start after it.

	Usage: csmith_runner <mcu> <elf> <heap_start_hex> [max_cycles]
	Exit:  0 OK, 2 CRASH, 3 TIMEOUT, 4 STACKOVF, 1 usage/load error.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sim_avr.h"
#include "sim_elf.h"
#include "sim_core.h"

#define PAINT		0xa5
#define GUARD_BYTES	32

static uint16_t op_at(avr_t *avr, uint32_t pc)
{
	if (pc + 1 > avr->flashend)
		return 0;
	return avr->flash[pc] | (avr->flash[pc + 1] << 8);
}

static int at_end_marker(avr_t *avr)
{
	return avr->pc >= 2 &&
		op_at(avr, avr->pc) == 0xcfff &&	/* rjmp .-2 */
		op_at(avr, avr->pc - 2) == 0x9598;	/* break    */
}

int main(int argc, char *argv[])
{
	if (argc < 4) {
		fprintf(stderr, "usage: %s <mcu> <elf> <heap_start_hex> [max_cycles]\n",
				argv[0]);
		return 1;
	}
	const char *mcu = argv[1];
	const char *elf = argv[2];
	uint32_t heap_start = strtoul(argv[3], NULL, 16) & 0xffff;
	uint64_t max_cycles = argc > 4 ? strtoull(argv[4], NULL, 0) : 200000000ull;

	avr_t *avr = avr_make_mcu_by_name(mcu);
	if (!avr) {
		fprintf(stderr, "unknown core %s\n", mcu);
		return 1;
	}
	elf_firmware_t f;
	memset(&f, 0, sizeof(f));
	if (elf_read_firmware(elf, &f)) {
		fprintf(stderr, "cannot read %s\n", elf);
		return 1;
	}
	avr_init(avr);
	avr->log = LOG_ERROR;
	avr_load_firmware(avr, &f);

	if (heap_start == 0 || heap_start > avr->ramend) {
		fprintf(stderr, "bad heap_start 0x%x (ramend 0x%x)\n",
				heap_start, avr->ramend);
		return 1;
	}
	for (uint32_t a = heap_start; a <= avr->ramend; a++)
		avr->data[a] = PAINT;

	int state = cpu_Running;
	while (!at_end_marker(avr)) {
		state = avr_run(avr);
		if (state == cpu_Done || state == cpu_Crashed) {
			printf("CRASH state=%d pc=0x%04x cycles=%llu\n", state,
				   avr->pc, (unsigned long long)avr->cycle);
			return 2;
		}
		if (avr->cycle > max_cycles) {
			printf("TIMEOUT cycles=%llu\n", (unsigned long long)avr->cycle);
			return 3;
		}
	}

	uint32_t lowest = avr->ramend + 1;	/* lowest byte the stack touched */
	for (uint32_t a = heap_start; a <= avr->ramend; a++)
		if (avr->data[a] != PAINT) {
			lowest = a;
			break;
		}
	if (lowest < heap_start + GUARD_BYTES) {
		printf("STACKOVF heap_start=0x%04x lowest=0x%04x\n", heap_start, lowest);
		return 4;
	}

	uint32_t crc = avr->data[26] | (avr->data[27] << 8) |
				   ((uint32_t)avr->data[30] << 16) |
				   ((uint32_t)avr->data[31] << 24);
	printf("OK crc=%08x cycles=%llu stack=%u\n", crc,
		   (unsigned long long)avr->cycle, avr->ramend + 1 - lowest);
	return 0;
}
