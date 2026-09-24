#!/usr/bin/env bash
#
# fuzz_one.sh SEED — generate, build and run one Csmith program on every
# configured core/optimisation pair, then compare the checksums.
#
# Normally driven by fuzz.sh, which exports the settings below. Prints one line:
#   <seed> <RESULT> <per-config detail...>
# RESULT is one of
#   PASS            all configs ran to completion with the same CRC
#   MISMATCH        all configs completed but the CRCs differ   -> saved
#   CRASH           a config crashed / hit an invalid state      -> saved
#   SKIP_SIZE       program does not fit flash/RAM on some config
#   SKIP_STACK      the stack ran into .bss/.data on some config
#   SKIP_TIMEOUT    a config exceeded MAX_CYCLES (long-running program)
#   SKIP_GEN        csmith failed to generate
#   BUILD_ERROR     avr-gcc failed for a reason other than size    -> saved
set -u

seed=$1
: "${FUZZ_DIR:?}" "${WORK:?}" "${CSMITH_INC:?}" "${RUNNER:?}"
: "${CONFIGS:?}" "${MAX_CYCLES:?}" "${WALL_TIMEOUT:?}" "${CSMITH_OPTS?}"

d="$WORK/tmp/$seed"
rm -rf "$d"
mkdir -p "$d"

save() {	# save <kind>: keep the case directory under findings/
	local dest="$WORK/findings/$seed-$1"
	rm -rf "$dest"
	mv "$d" "$dest"
}

# shellcheck disable=SC2086
if ! csmith --seed "$seed" $CSMITH_OPTS -o "$d/program.c" >/dev/null 2>&1; then
	echo "$seed SKIP_GEN"
	rm -rf "$d"
	exit 0
fi

detail=""
crcs=()
status=PASS
for cfg in $CONFIGS; do
	mcu=${cfg%%:*}
	opt=${cfg#*:}
	opt=${opt//,/ }		# commas separate extra flags: atmega328p:-Os,-mcall-prologues
	tag="$mcu${opt// /}"
	echo "$tag $mcu $opt" >>"$d/configs"
	elf="$d/$tag.elf"

	# shellcheck disable=SC2086
	if ! avr-gcc -mmcu="$mcu" $opt -w \
			-I "$FUZZ_DIR/runtime" -I "$CSMITH_INC" \
			-ffunction-sections -fdata-sections -Wl,--gc-sections \
			-o "$elf" "$d/program.c" "$FUZZ_DIR/runtime/fuzz_main.c" \
			>"$d/$tag.build.log" 2>&1; then
		if grep -qE "overflow|will not fit|region .* overflowed" "$d/$tag.build.log"; then
			echo "$seed SKIP_SIZE $tag"
			rm -rf "$d"
		else
			echo "$seed BUILD_ERROR $tag"
			save build_error
		fi
		exit 0
	fi

	heap=$(avr-nm "$elf" | awk '$3 == "_end" { print $1 }')
	out=$(timeout "$WALL_TIMEOUT" "$RUNNER" "$mcu" "$elf" "$heap" "$MAX_CYCLES" \
			2>"$d/$tag.err")
	rc=$?
	[ $rc -eq 124 ] && out="TIMEOUT wall=${WALL_TIMEOUT}s"
	echo "$out" >"$d/$tag.out"
	detail="$detail $tag:${out%% *}"

	case "$out" in
	OK*)
		crc=${out#*crc=}; crc=${crc%% *}
		crcs+=("$crc")
		detail="$detail=$crc"
		;;
	STACKOVF*)	[ "$status" = PASS ] && status=SKIP_STACK ;;
	TIMEOUT*)	[ "$status" = PASS ] && status=SKIP_TIMEOUT ;;
	*)		status=CRASH ;;
	esac
done

if [ "$status" = PASS ]; then
	for c in "${crcs[@]}"; do
		[ "$c" != "${crcs[0]}" ] && status=MISMATCH
	done
fi

echo "$seed $status$detail"
case "$status" in
MISMATCH)	save mismatch ;;
CRASH)		save crash ;;
*)		[ "${KEEP:-0}" = 1 ] && save "$(echo "$status" | tr 'A-Z' 'a-z')" || rm -rf "$d" ;;
esac
exit 0
