/*
	atmega4809_oled_spi.c

	Drives a 128x64 SSD1306 OLED over the modern AVR's SPI (SPI0) bus in 4-wire
	mode. It brings up SPI0 as a master plus three control GPIOs on PORTC
	(CS=PC0, DC=PC1, RST=PC2), runs the SSD1306 init sequence, then writes a
	framebuffer pattern (a checkerboard with a moving vertical bar).

	This is the SPI firmware for examples/board_atmega4809_oled; the host renders
	the panel in an OpenGL window. See atmega4809_oled_i2c.c for the I2C variant.

	Copyright 2026 simavr authors

 	This file is part of simavr.

	simavr is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	simavr is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with simavr.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <avr/io.h>
#include <util/delay.h>
#include <stdint.h>

#include "avr_mcu_section.h"
AVR_MCU(F_CPU, "atmega4809");

/* Control GPIOs on PORTC — must match the wiring in the host (oled.c). */
#define CS_bm	PIN0_bm		/* PC0: chip select (active low) */
#define DC_bm	PIN1_bm		/* PC1: data(1)/command(0) */
#define RST_bm	PIN2_bm		/* PC2: reset (active low) */

static void
spi_init(void)
{
	/* SPI0 default route: MOSI=PA4, SCK=PA6. Drive them (and SS/PA7) as out. */
	PORTA.DIRSET = PIN4_bm | PIN6_bm | PIN7_bm;
	SPI0.CTRLB = SPI_SSD_bm;			/* SS-disable: stay master */
	SPI0.CTRLA = SPI_ENABLE_bm | SPI_MASTER_bm;	/* master, full speed */

	/* control GPIOs */
	PORTC.DIRSET = CS_bm | DC_bm | RST_bm;
	PORTC.OUTSET = CS_bm | RST_bm;			/* CS idle high, RST de-asserted */
}

static void
spi_tx(uint8_t b)
{
	SPI0.DATA = b;
	uint16_t budget = 8000;
	while (budget-- && !(SPI0.INTFLAGS & SPI_IF_bm))
		;
	(void)SPI0.DATA;				/* read clears IF */
}

static void
oled_reset(void)
{
	PORTC.OUTSET = RST_bm;
	_delay_us(3);
	PORTC.OUTCLR = RST_bm;				/* pulse reset low */
	_delay_us(3);
	PORTC.OUTSET = RST_bm;
}

static void
oled_cmd(uint8_t c)
{
	PORTC.OUTCLR = DC_bm;				/* command */
	PORTC.OUTCLR = CS_bm;				/* select */
	spi_tx(c);
	PORTC.OUTSET = CS_bm;				/* deselect */
}

static void
oled_data(const uint8_t *buf, uint16_t n)
{
	PORTC.OUTSET = DC_bm;				/* data */
	PORTC.OUTCLR = CS_bm;
	while (n--)
		spi_tx(*buf++);
	PORTC.OUTSET = CS_bm;
}

static void
oled_init(void)
{
	oled_reset();
	oled_cmd(0xAE);			/* display off */
	oled_cmd(0xA8); oled_cmd(0x3F);	/* multiplex ratio = 1/64 */
	oled_cmd(0xD3); oled_cmd(0x00);	/* display offset = 0 */
	oled_cmd(0x40);			/* display start line = 0 */
	oled_cmd(0xA1);			/* segment remap */
	oled_cmd(0xC8);			/* COM scan direction remapped */
	oled_cmd(0xDA); oled_cmd(0x12);	/* COM pins config */
	oled_cmd(0x81); oled_cmd(0x7F);	/* contrast */
	oled_cmd(0xA4);			/* resume to RAM content */
	oled_cmd(0xA6);			/* normal (non-inverted) */
	oled_cmd(0x20); oled_cmd(0x00);	/* horizontal addressing mode */
	oled_cmd(0xD5); oled_cmd(0x80);	/* display clock divide */
	oled_cmd(0x8D); oled_cmd(0x14);	/* charge pump on */
	oled_cmd(0xAF);			/* display on */
}

static void
oled_blit(uint8_t bar_col)
{
	oled_cmd(0x21); oled_cmd(0); oled_cmd(127);	/* column window 0..127 */
	oled_cmd(0x22); oled_cmd(0); oled_cmd(7);	/* page window 0..7 */

	for (uint8_t page = 0; page < 8; page++) {
		uint8_t row[128];
		for (uint8_t col = 0; col < 128; col++) {
			uint8_t v = ((col ^ (page << 3)) & 8) ? 0xAA : 0x55;	/* checker */
			if (col == bar_col || col == (uint8_t)(bar_col + 1))
				v = 0xFF;					/* moving bar */
			row[col] = v;
		}
		oled_data(row, sizeof(row));
	}
}

int
main(void)
{
	spi_init();
	oled_init();

	uint8_t bar = 0;
	for (;;) {
		oled_blit(bar);
		bar = (uint8_t)(bar + 4);
		if (bar > 126)
			bar = 0;
		_delay_ms(40);
	}
}
