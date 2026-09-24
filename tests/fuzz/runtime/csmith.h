/*
	csmith.h — replacement Csmith runtime for simavr differential fuzzing.

	Generated Csmith programs start with  #include "csmith.h" ; the fuzz scripts
	put this directory ahead of Csmith's own include directory, so this header
	is used instead of the stock one. Differences from the stock AVR runtime:

	  - CRC32 is computed bitwise, with no 1 KB crc32_tab[] in RAM (the stock
	    table alone would use half of the RAM on a 2 KB part). The result is
	    bit-identical to the table version.
	  - transparent_crc()'s name argument is dropped by a macro, so the
	    per-variable string literals are never referenced. On classic AVR
	    they would otherwise be copied into RAM.
	  - main() is renamed to csmith_main(); runtime/fuzz_main.c provides a real
	    main() with well-defined argc/argv.
	  - platform_main_end() leaves the full 32-bit CRC in X (low) / Z (high)
	    and parks on  BREAK ; RJMP .-2  with interrupts off, which is the end
	    marker tests/fuzz/csmith_runner.c looks for.

	The safe-math wrappers still come from Csmith's own safe_math.h.
 */
#ifndef CSMITH_FUZZ_RUNTIME_H
#define CSMITH_FUZZ_RUNTIME_H

#include <stdint.h>
#include <limits.h>
#include <string.h>
#include <float.h>
#include <math.h>

#define COUNT(n)
#define TC
#define SAFE
#define UNDEFINED(__val) (__val)
#define LOG_INDEX
#define LOG_EXEC
#define STATIC static
#define assert(x) ((void)0)
#define printf(...) ((void)0)
#define fprintf(...) ((void)0)

#define FUNC_NAME(x) (safe_##x)
#include "safe_math.h"
#undef FUNC_NAME

#define INT_BIT (sizeof(int) * CHAR_BIT)
#define _CSMITH_BITFIELD(x) (((x) > INT_BIT) ? ((x) % INT_BIT) : (x))

#define main csmith_main

/* FUZZ_SELFTEST_CORRUPT perturbs the CRC seed so a config built with it must
 * be reported as MISMATCH; it exists only to prove the harness detects one. */
#ifdef FUZZ_SELFTEST_CORRUPT
static uint32_t crc32_context = 0xFFFFFFFEUL;
#else
static uint32_t crc32_context = 0xFFFFFFFFUL;
#endif

static void crc32_gentab(void) { /* table-less CRC: nothing to build */ }

static void platform_main_begin(void) { }

static void crc32_byte(uint8_t b)
{
	uint32_t c = crc32_context ^ b;
	for (uint8_t i = 0; i < 8; i++)
		c = (c >> 1) ^ (0xEDB88320UL & -(c & 1));
	crc32_context = c;
}

#if defined(__SPLAT__) || defined(NO_LONGLONG)
static void fuzz_crc_val(uint32_t val)
{
	for (uint8_t i = 0; i < 4; i++)
		crc32_byte((val >> (8 * i)) & 0xff);
}
#else
static void fuzz_crc_val(uint64_t val)
{
	for (uint8_t i = 0; i < 8; i++)
		crc32_byte((val >> (8 * i)) & 0xff);
}
#endif

static void fuzz_crc_bytes(char *ptr, int nbytes)
{
	for (int i = 0; i < nbytes; i++)
		crc32_byte(ptr[i]);
}

#define transparent_crc(val, vname, flag) fuzz_crc_val(val)
#define transparent_crc_bytes(ptr, nbytes, vname, flag) fuzz_crc_bytes(ptr, nbytes)

static void platform_main_end(uint32_t crc, int flag)
{
	(void)flag;
	__asm__ volatile(
		"cli"            "\n\t"
		"mov r26, %A0"   "\n\t"
		"mov r27, %B0"   "\n\t"
		"mov r30, %C0"   "\n\t"
		"mov r31, %D0"   "\n\t"
		"break"          "\n\t"
		"1: rjmp 1b"     "\n\t"
		:
		: "r"(crc)
		: "r26", "r27", "r30", "r31", "memory");
}

#endif /* CSMITH_FUZZ_RUNTIME_H */
