#
# This makefile takes each "at*" file, extracts its part name
# and compiles it into an ELF binary.
#
# 	Copyright 2008-2012 Michel Pollet <buserror@gmail.com>
#
#	This file is part of simavr.
#
#	simavr is free software: you can redistribute it and/or modify
#	it under the terms of the GNU General Public License as published by
#	the Free Software Foundation, either version 3 of the License, or
#	(at your option) any later version.
#
#	simavr is distributed in the hope that it will be useful,
#	but WITHOUT ANY WARRANTY; without even the implied warranty of
#	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#	GNU General Public License for more details.
#
#	You should have received a copy of the GNU General Public License
#	along with simavr.  If not, see <http://www.gnu.org/licenses/>.

sources		:= $(wildcard at*.c)
simavr 		= ..

IPATH 		+= ${simavr}/include
IPATH 		+= ${simavr}/simavr/sim

tests_src	:= ${wildcard test_*.c}
tests_src	:= $(filter-out test_attiny3217_%,$(tests_src))

all: obj axf hex tst

include ../Makefile.common

HAVE_ATTINY3217_AVR_GCC := $(shell ${AVR}gcc -mmcu=attiny3217 -E - < /dev/null >/dev/null 2>&1 && echo 1 || echo 0)

# The firmware-backed ATtiny3217 tests can run when avr-gcc supports
# -mmcu=attiny3217. Keep them enabled automatically on capable toolchains,
# but still build on older environments by filtering them out there.
ifeq ($(HAVE_ATTINY3217_AVR_GCC),0)
sources		:= $(filter-out attiny3217_%,$(sources))
endif

tst: ${patsubst %.c, ${OBJ}/%.tst, ${tests_src}}

axf: ${sources:.c=.axf}

hex: atmega88_example.hex

${OBJ}/%.tst: tests.c %.c
ifneq ($(E),)
	@echo TST $@
endif
	${E}$(CC) -MMD ${CPPFLAGS} ${CFLAGS} ${LFLAGS} -o $@ ${patsubst %.h,, ${^}} $(LDFLAGS)

run_tests: all
	@export LD_LIBRARY_PATH=${simavr}/simavr/${OBJ} ;\
	num_failed=0 ;\
	num_run=0 ;\
	for test in ${OBJ}/test_*.tst; do \
	    num_run=$$(($$num_run+1)) ;\
	    if ! $$test; then \
			echo "$$test returned with exit value $$?." ;\
			num_failed=$$(($$num_failed+1)) ;\
	    fi ;\
	done ;\
	if [ "$(HAVE_ATTINY3217_AVR_GCC)" = "1" ]; then \
	    for target in attiny3217-demo attiny3217-selftest; do \
	        num_run=$$(($$num_run+1)) ;\
	        if ! $(MAKE) $$target >/dev/null; then \
				echo "$$target returned with exit value $$?." ;\
				num_failed=$$(($$num_failed+1)) ;\
	        fi ;\
	    done ;\
	fi ;\
	echo "Tests run: $$num_run  Successes: $$(($$num_run-$$num_failed))  Failures: $$num_failed" ;\
	exit $$num_failed

# ATtiny3217 modern-AVR blink demo (needs a modern avr-gcc, see note above).
# Builds the firmware ELF and a host program that loads it, runs it on simavr,
# and verifies the LED on PA0 toggles.
attiny3217-demo: attiny3217_blink.axf ${OBJ}/test_attiny3217_blink
	@echo "--- ATtiny3217 blink demo ---"
	@LD_LIBRARY_PATH=${simavr}/simavr/${OBJ} ${OBJ}/test_attiny3217_blink attiny3217_blink.axf

${OBJ}/test_attiny3217_blink: test_attiny3217_blink.c | ${OBJ}
	$(CC) ${CPPFLAGS} -I${simavr}/simavr/sim -I${simavr}/simavr/cores \
		-o $@ $< -L${simavr}/simavr/${OBJ} -lsimavr -lelf -lm

# ATtiny3217 modern-AVR self-test: real compiled firmware that exercises the
# deepened peripherals (EEPROM, temp sensor, TCB0, TCA0) and reports a pass
# bitmask the host harness checks.
attiny3217-selftest: attiny3217_selftest.axf ${OBJ}/test_attiny3217_selftest
	@echo "--- ATtiny3217 self-test ---"
	@LD_LIBRARY_PATH=${simavr}/simavr/${OBJ} ${OBJ}/test_attiny3217_selftest attiny3217_selftest.axf

${OBJ}/test_attiny3217_selftest: test_attiny3217_selftest.c | ${OBJ}
	$(CC) ${CPPFLAGS} -I${simavr}/simavr/sim -I${simavr}/simavr/cores \
		-o $@ $< -L${simavr}/simavr/${OBJ} -lsimavr -lelf -lm

clean: clean-${OBJ}
	rm -f *.axf *.hex *.vcd
