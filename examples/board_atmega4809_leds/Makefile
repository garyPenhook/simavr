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

target = ledramp
firmware = atmega4809_ledramp.axf
simavr = ../..

IPATH = .
IPATH += ${simavr}/include
IPATH += ${simavr}/simavr/sim

VPATH = .

LDFLAGS += -lpthread

# OpenGL window (GLUT). Pulls in -lglut -lGL -lGLU on Linux / the frameworks on
# macOS, like the other visual example boards (board_ledramp, board_hd44780).
include ../Makefile.opengl

# The atmega4809 firmware needs a modern avr-gcc (12+) with the megaAVR-0 device
# headers. Gate the firmware build on that toolchain so a plain `make` across
# examples/ still succeeds on older toolchains; install a modern avr-gcc to
# build + run the demo.
AVR_4809_OK := ${shell echo 'int main(void){return 0;}' | \
	$(if $(AVR),$(AVR),avr-)gcc -mmcu=atmega4809 -x c -c -o /dev/null - \
	>/dev/null 2>&1 && echo yes}

ifeq (${AVR_4809_OK}, yes)
all: obj ${firmware} ${target}
else
all: obj ${target}
	@echo "NOTE: avr-gcc cannot target atmega4809 (needs avr-gcc 12+);"
	@echo "      skipping firmware build. Install a modern avr-gcc, then"
	@echo "      're-run 'make' to build atmega4809_ledramp.axf and run ./ledramp."
endif

include ${simavr}/Makefile.common

board = ${OBJ}/${target}.elf

${board}: ${OBJ}/${target}.o

${target}: ${board}
	@echo $@ done

clean: clean-${OBJ}
	rm -rf *.hex *.a *.axf ${target} *.vcd
