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

target = oled
simavr = ../..

IPATH = .
IPATH += ../parts
IPATH += ${simavr}/include
IPATH += ${simavr}/simavr/sim

VPATH = .
VPATH += ../parts

LDFLAGS += -lpthread

# OpenGL window (GLUT), like the classic board_ssd1306 / board_ledramp.
include ../Makefile.opengl

# Two firmwares from the same board: the SSD1306 over I2C and over SPI. The
# generic %.axf rule in Makefile.common derives -mmcu from the filename prefix
# (atmega4809_*), so both build as atmega4809.
firmwares = atmega4809_oled_i2c.axf atmega4809_oled_spi.axf

# Gate the firmware build on a modern avr-gcc (12+) with the megaAVR-0 headers,
# so a plain `make` across examples/ still succeeds on older toolchains; install
# a modern avr-gcc to build + run the demo.
AVR_4809_OK := ${shell echo 'int main(void){return 0;}' | \
	$(if $(AVR),$(AVR),avr-)gcc -mmcu=atmega4809 -x c -c -o /dev/null - \
	>/dev/null 2>&1 && echo yes}

ifeq (${AVR_4809_OK}, yes)
all: obj ${firmwares} ${target}
else
all: obj ${target}
	@echo "NOTE: avr-gcc cannot target atmega4809 (needs avr-gcc 12+);"
	@echo "      skipping firmware build. Install a modern avr-gcc, then"
	@echo "      're-run 'make' to build the .axf files and run ./oled."
endif

include ${simavr}/Makefile.common

board = ${OBJ}/${target}.elf

${board}: ${OBJ}/ssd1306_virt.o
${board}: ${OBJ}/ssd1306_glut.o
${board}: ${OBJ}/${target}.o

${target}: ${board}
	@echo $@ done

clean: clean-${OBJ}
	rm -rf *.hex *.a *.axf ${target} *.vcd
