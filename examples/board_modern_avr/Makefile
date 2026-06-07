#
#	Copyright 2026 simavr authors
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

# Generic "all supported modern cores" coverage board. One firmware source
# (blink_fw.c) is compiled once per device into fw_<device>.axf, and the host
# runner loads + smoke-tests each.

target = runner
simavr = ../..

IPATH = .
IPATH += ${simavr}/include
IPATH += ${simavr}/simavr/sim

VPATH = .

# Every modern core this fork supports: 15 tinyAVR 1-series + 8 megaAVR 0-series.
# Keep in sync with simavr/cores/sim_{tiny,mega}*.c (the sim_tinyx1.h / sim_megax08.h users).
TINY_MCUS = attiny212 attiny214 attiny412 attiny414 attiny416 attiny417 \
            attiny814 attiny816 attiny817 \
            attiny1614 attiny1616 attiny1617 \
            attiny3214 attiny3216 attiny3217
MEGA_MCUS = atmega808 atmega809 atmega1608 atmega1609 \
            atmega3208 atmega3209 atmega4808 atmega4809
ALL_MCUS  = ${TINY_MCUS} ${MEGA_MCUS}

AVRGCC = $(if $(AVR),$(AVR),avr-)gcc

# Filter to the cores the installed toolchain can actually build (needs a
# modern avr-gcc 12+ with the tinyAVR-1 / megaAVR-0 device headers). On an
# older toolchain MCUS_OK is empty: no firmware is built and `make` still
# succeeds (the host runner builds either way); `make run` then exercises
# whatever fw_*.axf are already present in this directory.
MCUS_OK := $(foreach m,$(ALL_MCUS),\
    $(if $(shell echo 'int main(void){return 0;}' | \
        $(AVRGCC) -mmcu=$(m) -x c -c -o /dev/null - >/dev/null 2>&1 && echo y),$(m)))

firmwares = $(patsubst %,fw_%.axf,$(MCUS_OK))

all: obj $(firmwares) ${target}
ifeq ($(strip $(MCUS_OK)),)
	@echo "NOTE: avr-gcc cannot target any modern AVR (needs avr-gcc 12+);"
	@echo "      skipping firmware builds. Install a modern avr-gcc, then"
	@echo "      're-run 'make' to build fw_*.axf for every supported core."
endif

include ${simavr}/Makefile.common

# Build the device-agnostic firmware once per MCU. This pattern rule is more
# specific (shorter stem) than Makefile.common's generic %.axf:%.c, and only it
# has a valid prerequisite (blink_fw.c), so it is the one selected.
fw_%.axf: blink_fw.c
	@echo AVR-CC $@ '(-mmcu=$*)'
	$(AVRGCC) -Wall -gdwarf-2 -Os -std=gnu99 \
		-mmcu=$* -DF_CPU=8000000 -DMCU_NAME='"$*"' \
		-fno-inline-small-functions -ffunction-sections -fdata-sections \
		-Wl,--relax,--gc-sections \
		-Wl,--undefined=_mmcu,--section-start=.mmcu=0x910000 \
		-I$(SIMAVR)/sim/avr \
		blink_fw.c -o $@
	@${AVR}size $@ | sed '1d'

board = ${OBJ}/${target}.elf

${board}: ${OBJ}/${target}.o

${target}: ${board}
	@echo $@ done

# Smoke-test every firmware present in this directory through the host runner.
run: all
	@./${OBJ}/${target}.elf $(sort $(wildcard fw_*.axf))

clean: clean-${OBJ}
	rm -rf *.hex *.a *.axf ${target} *.vcd
