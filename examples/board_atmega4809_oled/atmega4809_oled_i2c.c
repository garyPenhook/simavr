/*
	atmega4809_oled_i2c.c

	Drives a 128x64 SSD1306 OLED over the modern AVR's I2C (TWI0) bus. It brings
	up TWI0 as a master, runs the SSD1306 power-on/init sequence, then writes a
	framebuffer pattern (a checkerboard with a moving vertical bar) page by page.

	This is the I2C firmware for examples/board_atmega4809_oled; the host renders
	the panel in an OpenGL window. See atmega4809_oled_spi.c for the SPI variant.

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

#define SSD1306_ADDR	0x3C		/* 7-bit I2C address */
#define OLED_CMD	0x00		/* control byte: command stream */
#define OLED_DATA	0x40		/* control byte: data stream */

/* --- TWI0 master, polled, with bounded waits so a stall can never hang --- */

static void
twi_init(void)
{
	/* ~100 kHz-ish; exact rate is irrelevant in simulation. */
	TWI0.MBAUD = 10;
	TWI0.MCTRLA = TWI_ENABLE_bm;
	TWI0.MSTATUS = TWI_BUSSTATE_IDLE_gc;	/* force the bus to a known idle */
}

static uint8_t
twi_wait_w(void)
{
	uint16_t budget = 8000;
	while (budget-- && !(TWI0.MSTATUS & (TWI_WIF_bm | TWI_RIF_bm)))
		;
	return (TWI0.MSTATUS & TWI_WIF_bm) != 0;
}

static void
twi_start_write(uint8_t addr7)
{
	TWI0.MADDR = (uint8_t)(addr7 << 1) | 0;		/* address + write */
	twi_wait_w();
}

static void
twi_write(uint8_t b)
{
	TWI0.MDATA = b;
	twi_wait_w();
}

static void
twi_stop(void)
{
	TWI0.MCTRLB = TWI_MCMD_STOP_gc;
}

/* Send one command byte (own transaction; simple and robust). */
static void
oled_cmd(uint8_t c)
{
	twi_start_write(SSD1306_ADDR);
	twi_write(OLED_CMD);
	twi_write(c);
	twi_stop();
}

static void
oled_init(void)
{
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

/* Point the SSD1306 at the full 128x64 window for a horizontal write. */
static void
oled_window_full(void)
{
	oled_cmd(0x21); oled_cmd(0); oled_cmd(127);	/* column 0..127 */
	oled_cmd(0x22); oled_cmd(0); oled_cmd(7);	/* page 0..7 */
}

/* Stream the whole framebuffer: 8 pages x 128 columns, one data byte each. */
static void
oled_blit(uint8_t bar_col)
{
	oled_window_full();
	for (uint8_t page = 0; page < 8; page++) {
		twi_start_write(SSD1306_ADDR);
		twi_write(OLED_DATA);
		for (uint8_t col = 0; col < 128; col++) {
			uint8_t v = ((col ^ (page << 3)) & 8) ? 0xAA : 0x55;	/* checker */
			if (col == bar_col || col == (uint8_t)(bar_col + 1))
				v = 0xFF;					/* moving bar */
			twi_write(v);
		}
		twi_stop();
	}
}

int
main(void)
{
	twi_init();
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
