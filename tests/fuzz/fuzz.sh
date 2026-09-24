#!/usr/bin/env bash
#
# fuzz.sh — Csmith differential fuzzing of the simavr instruction engine.
#
# Each seed becomes one random C program, built for every entry in CONFIGS
# (core:optimisation) and run in simavr; all CRCs must agree. See README.md.
#
#   tests/fuzz/fuzz.sh [-n COUNT] [-j JOBS] [-s FIRST_SEED]
#
#   -n COUNT       number of seeds to try (0 = run until Ctrl-C; default 0)
#   -j JOBS        parallel workers (default: nproc)
#   -s FIRST_SEED  first seed; seeds are FIRST_SEED, FIRST_SEED+1, ...
#                  (default: derived from the clock)
#
# Environment overrides (defaults shown in the code below):
#   CONFIGS, MAX_CYCLES, WALL_TIMEOUT, CSMITH_OPTS, CSMITH_INC, WORK, KEEP=1
set -euo pipefail

FUZZ_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
TOP=$(cd "$FUZZ_DIR/../.." && pwd)

count=0
jobs=$(nproc)
first=$(( $(date +%s) % 1000000 * 1000 ))
while getopts "n:j:s:h" o; do
	case $o in
	n) count=$OPTARG ;;
	j) jobs=$OPTARG ;;
	s) first=$OPTARG ;;
	*) sed -n '2,20p' "$0"; exit 1 ;;
	esac
done

# Reference core first: every other config is compared against the same CRC.
: "${CONFIGS:=atmega328p:-Os attiny3217:-Os attiny3217:-O1 attiny3217:-O0 atmega4809:-Os}"
: "${MAX_CYCLES:=200000000}"
: "${WALL_TIMEOUT:=120}"
: "${CSMITH_OPTS:=--no-packed-struct --no-bitfields --max-funcs 4 --max-array-dim 2 --max-array-len-per-dim 4 --max-struct-fields 4 --max-block-depth 3 --max-expr-complexity 6}"
: "${WORK:=$FUZZ_DIR/work}"
if [ -z "${CSMITH_INC:-}" ]; then
	csmith_bin=$(command -v csmith || true)
	[ -n "$csmith_bin" ] || { echo "csmith not found in PATH" >&2; exit 1; }
	CSMITH_INC=$(cd "$(dirname "$(readlink -f "$csmith_bin")")/../include" && pwd)
fi

for tool in csmith avr-gcc avr-nm timeout; do
	command -v "$tool" >/dev/null || { echo "missing tool: $tool" >&2; exit 1; }
done
[ -f "$CSMITH_INC/safe_math.h" ] || {
	echo "safe_math.h not found in $CSMITH_INC (set CSMITH_INC)" >&2; exit 1; }
for cfg in $CONFIGS; do
	mcu=${cfg%%:*}
	echo 'int main(void){return 0;}' | avr-gcc -mmcu="$mcu" -x c -o /dev/null - 2>/dev/null ||
		{ echo "avr-gcc cannot build for $mcu" >&2; exit 1; }
done

# Build the library (incremental) and the runner against the static lib, so
# the fuzzer always tests the current tree.
make -s -C "$TOP/simavr" >/dev/null
OBJ=$(ls -d "$TOP"/simavr/obj-* | head -1)
mkdir -p "$WORK/tmp" "$WORK/findings"
RUNNER="$WORK/csmith_runner"
cc -O2 -I "$TOP/simavr/sim" -I "$TOP/simavr/cores" -o "$RUNNER" \
	"$FUZZ_DIR/csmith_runner.c" "$OBJ/libsimavr.a" -lelf -lm

for mcu in $(for cfg in $CONFIGS; do echo "${cfg%%:*}"; done | sort -u); do
	if "$RUNNER" "$mcu" /dev/null 0 2>&1 | grep -q "unknown core"; then
		echo "simavr has no '$mcu' core (see run_avr --list-cores)" >&2
		exit 1
	fi
done

export FUZZ_DIR WORK CSMITH_INC RUNNER CONFIGS MAX_CYCLES WALL_TIMEOUT CSMITH_OPTS
export KEEP="${KEEP:-0}"

LOG="$WORK/results.log"
echo "# $(date -Is) first_seed=$first count=$count jobs=$jobs" >>"$LOG"
echo "# CONFIGS=$CONFIGS" >>"$LOG"
echo "fuzzing: seeds from $first, $jobs jobs, log $LOG, findings $WORK/findings/"

summary() {
	[ -f "$LOG" ] || return 0
	echo
	echo "== summary (this log: $LOG) =="
	grep -v '^#' "$LOG" | awk '{ n[$2]++ } END { for (k in n) printf "  %-13s %d\n", k, n[k] }' | sort
	ls "$WORK/findings" 2>/dev/null | sed 's/^/  finding: /'
}
trap summary EXIT
trap 'exit 130' INT TERM

if [ "$count" -gt 0 ]; then
	seq "$first" $((first + count - 1))
else
	seq "$first" inf
fi | xargs -P "$jobs" -I{} "$FUZZ_DIR/fuzz_one.sh" {} | tee -a "$LOG"
