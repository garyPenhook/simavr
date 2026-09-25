# Csmith differential fuzzing of the simavr engine

Generates random, well-defined C programs with [Csmith](https://github.com/csmith-project/csmith),
builds each one for several core / optimisation configurations, runs them all in
simavr, and checks that every run ends with the same CRC of the program's global
state. A disagreement means a bug in the simavr instruction engine or in avr-gcc.

## What it covers, and what it doesn't

It **covers** the shared instruction decoder/executor (`sim_core.c`) and the
modern-AVR `arch` paths the fork added:
- SP/SREG at their modern addresses
- the I/O offset
- AVRxt timing mode, which must not change results
- const data read through the flash-mapped data window on tinyAVR-1 / megaAVR-0

The classic `atmega328p` core is the reference: every other configuration must
match its CRC.

It **does not cover** peripherals, interrupts, sleep or reset: Csmith programs
are pure computation. See `doc/code_review_2026-09-24.md` for the targeted tests
those need.

## Requirements

- `csmith` in `PATH`. Its include directory, which must contain `safe_math.h`,
  is found next to the binary (`<prefix>/include`). Override it with
  `CSMITH_INC=...`.
- avr-gcc with support for every configured MCU (avr-gcc 12+ for
  `attiny3217` / `atmega4809`).
- `creduce` or `cvise`, only for `reduce.sh`.
- The simavr tree. `fuzz.sh` runs `make -C simavr` itself and links the runner
  against `libsimavr.a`, so it always tests the current sources.

## Running

```sh
tests/fuzz/fuzz.sh                 # run until Ctrl-C, one worker per CPU
tests/fuzz/fuzz.sh -n 1000         # 1000 seeds, then print a summary
tests/fuzz/fuzz.sh -n 200 -j 4 -s 12345   # 4 workers, seeds 12345..12544
```

Each seed prints one line and is appended to `tests/fuzz/work/results.log`:

```
5047 PASS atmega328p-Os:OK=372886eb attiny3217-Os:OK=372886eb ...
```

| Result | Meaning | Kept in `work/findings/`? |
|---|---|---|
| `PASS` | all configs finished with the same CRC | no |
| `MISMATCH` | all finished, CRCs differ: **a bug** | yes |
| `CRASH` | a config crashed / stopped / failed to run: **a bug** | yes |
| `BUILD_ERROR` | avr-gcc failed for a non-size reason (possible compiler bug, or a Csmith/runtime issue) | yes |
| `SKIP_SIZE` | the program doesn't fit flash/RAM for some config | no |
| `SKIP_STACK` | the stack grew into `.data/.bss` for some config | no |
| `SKIP_TIMEOUT` | a config ran past `MAX_CYCLES` (long-running program) | no |

Throughput reference: 48 seeds in ~50 s on 6 cores with the default five
configs, about 83 % of them giving a usable comparison.

### Settings (environment variables)

| Variable | Default | Notes |
|---|---|---|
| `CONFIGS` | `atmega328p:-Os attiny3217:-Os attiny3217:-O1 attiny3217:-O0 atmega4809:-Os` | `mcu:flags`; separate extra flags with commas, e.g. `atmega4809:-O2,-mcall-prologues`. |
| `MAX_CYCLES` | `200000000` | Simulated-cycle budget per run. |
| `WALL_TIMEOUT` | `120` | Seconds per run (catches simulator hangs). |
| `CSMITH_OPTS` | small programs, no bitfields, no packed structs | See `fuzz.sh`. Bitfields are off because Csmith assumes 32-bit `int` bitfield widths, which avr-gcc rejects. |
| `KEEP=1` | off | Also keep passing/skipped case directories (debugging). |
| `WORK` | `tests/fuzz/work` | Output directory (git-ignored). |

## Triage

A finding directory holds:
- `program.c`
- `configs`: tag, mcu and flags for each configuration
- `<tag>.elf`
- `<tag>.out`: the runner's status line
- `<tag>.err` and `<tag>.build.log`

Reproduce one config by hand:

```sh
d=tests/fuzz/work/findings/<seed>-mismatch
tests/fuzz/work/csmith_runner attiny3217 $d/attiny3217-Os.elf \
    $(avr-nm $d/attiny3217-Os.elf | awk '$3=="_end"{print $1}')
```

Shrink it with C-Reduce while it keeps failing:

```sh
tests/fuzz/reduce.sh tests/fuzz/work/findings/<seed>-mismatch
# or choose the pair explicitly:
tests/fuzz/reduce.sh <dir> atmega328p-Os attiny3217-Os
```

The result is `<dir>/reduce/program.c`. The interestingness test rejects
candidates with warnings that usually mean C-Reduce introduced undefined
behaviour, but the filter is only heuristic. Before filing a bug, read the
reduced program and confirm it is still well defined.

Triage before reducing. Rebuild the program at the disagreeing optimisation
levels on the other cores, including the classic `atmega328p`:
- If every core gives the same CRC at a given `-O` level, and the CRC only
  changes with `-O` (or with the codegen family, avr5 vs. avrxmega3), the
  simulator is consistent. The cause is avr-gcc or undefined behaviour in the
  generated program under AVR's 16-bit `int`.
- If two cores built for the same family (e.g. `attiny3217` and `atmega4809`,
  both avrxmega3) disagree at the same `-O` level, suspect simavr. To find the
  first wrong value, log each argument to `fuzz_crc_val` (the `transparent_crc`
  sequence) on both cores and diff the two lists.

To decide where the bug is, compare the two builds' disassembly
(`avr-objdump -d`):
- If both builds compute the value the C code specifies and simavr still
  disagrees, the bug is in **simavr**.
- If one build is simply wrong code, the bug is in **avr-gcc**. This is most
  likely between `atmega328p` and the AVRxt parts, since they use different
  code-generation paths.

## Findings

- **2026-09-24, seed 292515608 (fixed in `df4f262`).** On `atmega4809` at
  `-O0`, `const` data in the upper 16 KB of flash came back as code bytes.
  The engine wrapped mapped-flash reads with `& flashend`, which is wrong for
  the 48 KB ATmega4808/4809. The default configs build `atmega4809` only at
  `-Os`, where programs rarely reach that far into flash. Add
  `atmega4809:-O0` to `CONFIGS` to cover large images.
- The first ~10k seeds also gave 18 mismatches that follow the optimisation
  level on every core, the classic `atmega328p` included. Per the triage rule
  above, these are not simavr bugs.

## Self-test

Prove the harness catches a mismatch: a config built with
`-DFUZZ_SELFTEST_CORRUPT` uses a different CRC seed, so it must always report
`MISMATCH`:

```sh
CONFIGS="attiny3217:-Os attiny3217:-Os,-DFUZZ_SELFTEST_CORRUPT" \
    tests/fuzz/fuzz.sh -n 3
tests/fuzz/reduce.sh tests/fuzz/work/findings/<seed>-mismatch   # reduces to ~2 lines
```

## How it works

- `runtime/csmith.h` replaces Csmith's runtime header. It computes the CRC
  bitwise, with no 1 KB table in RAM. It drops the per-variable name strings,
  renames `main` to `csmith_main` (the real `main` is in
  `runtime/fuzz_main.c`), and ends the program with `cli; break; rjmp .-2`,
  with the CRC in X (low word) and Z (high word).
- `csmith_runner.c` is a small libsimavr host. It paints free RAM above `_end`,
  runs the program until the core parks on that end marker, checks for stack
  overflow, and prints `OK crc=...`, `TIMEOUT`, `CRASH` or `STACKOVF`.
- `fuzz_one.sh` handles one seed: generate, build every config, run, classify.
- `fuzz.sh` handles setup, the parallel driver (`xargs -P`), the log and the
  summary.
- `reduce.sh` builds a C-Reduce interestingness test for a finding and runs it.
