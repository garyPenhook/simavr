/*
	avr_ssd1306.h

	A 128x64 SSD1306 OLED controller model for the host API: I2C (slave
	address 0x3C/0x3D) or 4-wire SPI (CS, D/C, RES pins), GDDRAM with the
	three addressing modes, and the display state a renderer needs. Pure
	state, no UI: sim_host exposes a snapshot (avr_host_oled_read) and the
	Python GUI draws it.

	Behaviour follows the Solomon Systech SSD1306 data sheet Rev 1.0
	(command table 9-1, sections 8.1.5 and 10.1). Not modelled: scrolling
	(the commands are accepted and ignored), display offset (D3h), multiplex
	ratio, charge pump, timing/busy. The scroll-setup commands 26h/27h take
	six argument bytes, as drivers send them (Rev 1.0 lists four; the
	SSD1315 successor lists six).

	Unlike examples/parts/ssd1306_virt.c it resets into page addressing mode
	and never prints or aborts.
 */

#ifndef __AVR_SSD1306_H__
#define __AVR_SSD1306_H__

#include <stdint.h>
#include "sim_avr.h"

#define AVR_SSD1306_COLUMNS	128
#define AVR_SSD1306_PAGES	8

enum { AVR_SSD1306_ADDR_HORZ = 0, AVR_SSD1306_ADDR_VERT, AVR_SSD1306_ADDR_PAGE };

/* What a renderer needs. 'seq' changes whenever anything here changes. */
typedef struct avr_ssd1306_state_t {
	uint32_t	seq;
	uint8_t		on;			/* AFh/AEh */
	uint8_t		all_on;		/* A5h: ignore RAM */
	uint8_t		inverse;	/* A7h */
	uint8_t		remap;		/* A1h: column 127 is SEG0 */
	uint8_t		com_flip;	/* C8h: COM scan remapped */
	uint8_t		contrast;
	uint8_t		start_line;	/* 40h..7Fh, 0-63 */
	uint8_t		addr_mode;	/* AVR_SSD1306_ADDR_* */
	uint8_t		vram[AVR_SSD1306_PAGES][AVR_SSD1306_COLUMNS];
} avr_ssd1306_state_t;

typedef struct avr_ssd1306_t avr_ssd1306_t;

/* Wire an SSD1306 to the TWI bus 'bus' (modern cores: the char '0', '1').
 * addr7 is 0x3C or 0x3D. 'res' (port, pin) is optional: port 0 = none. */
avr_ssd1306_t * avr_ssd1306_new_twi(avr_t * avr, char bus, uint8_t addr7,
		char res_port, uint8_t res_pin);
/* 4-wire SPI on bus 'bus'. cs_port 0 = always selected. */
avr_ssd1306_t * avr_ssd1306_new_spi(avr_t * avr, char bus,
		char cs_port, uint8_t cs_pin, char dc_port, uint8_t dc_pin,
		char res_port, uint8_t res_pin);
/* Copy the current state. Call with the avr lock held (host does). */
void avr_ssd1306_snapshot(const avr_ssd1306_t * p, avr_ssd1306_state_t * out);
/* Unregister the part's IRQ callbacks and free it. Call while the avr is
 * still alive (before avr_terminate()). The part's IRQs stay in the avr's
 * pool and stay connected to the pins, now harmlessly. */
void avr_ssd1306_free(avr_ssd1306_t * p);

#endif
