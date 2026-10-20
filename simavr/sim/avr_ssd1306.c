/*
	avr_ssd1306.c

	See avr_ssd1306.h.
 */

#include <stdlib.h>
#include <string.h>
#include "avr_ssd1306.h"
#include "avr_ioport.h"
#include "avr_spi.h"
#include "avr_twi.h"
#include "sim_irq.h"

enum { IRQ_SPI_IN, IRQ_CS, IRQ_DC, IRQ_RES, IRQ_TWI_OUT, IRQ_TWI_IN, IRQ_COUNT };

struct avr_ssd1306_t {
	avr_t *			avr;
	avr_irq_t *		irq;
	avr_ssd1306_state_t	s;

	uint8_t			col, page, col_start, col_end, page_start, page_end;

	uint8_t			cmd;		/* multi-byte command in progress */
	uint8_t			args_left, args_n;
	uint8_t			args[6];

	/* bus */
	uint8_t			is_twi;
	uint8_t			addr7;
	uint8_t			selected;	/* TWI: we were addressed */
	uint8_t			peer;		/* TWI: address byte the master sent */
	uint8_t			expect_ctrl;	/* TWI: next byte is a control byte */
	uint8_t			co;			/* TWI: control byte had Co set */
	uint8_t			dc;			/* data (1) or command (0) */
	uint8_t			cs_high;	/* SPI chip select deasserted */
	uint8_t			have_cs;
	uint8_t			wired[IRQ_COUNT];
};

static void
reset(avr_ssd1306_t * p)
{
	memset(p->s.vram, 0, sizeof(p->s.vram));
	p->s.on = p->s.all_on = p->s.inverse = p->s.remap = p->s.com_flip = 0;
	p->s.contrast = 0x7F;
	p->s.start_line = 0;
	p->s.addr_mode = AVR_SSD1306_ADDR_PAGE;
	p->col = p->page = p->col_start = p->page_start = 0;
	p->col_end = AVR_SSD1306_COLUMNS - 1;
	p->page_end = AVR_SSD1306_PAGES - 1;
	p->cmd = 0;
	p->args_left = 0;
	p->s.seq++;
}

static void
write_data(avr_ssd1306_t * p, uint8_t b)
{
	p->s.vram[p->page & 7][p->col & 127] = b;
	switch (p->s.addr_mode) {
		case AVR_SSD1306_ADDR_HORZ:
			if (p->col >= p->col_end) {
				p->col = p->col_start;
				p->page = p->page >= p->page_end ? p->page_start : p->page + 1;
			} else
				p->col++;
			break;
		case AVR_SSD1306_ADDR_VERT:
			if (p->page >= p->page_end) {
				p->page = p->page_start;
				p->col = p->col >= p->col_end ? p->col_start : p->col + 1;
			} else
				p->page++;
			break;
		default:	/* page mode: wraps within the page, page unchanged */
			p->col = p->col >= AVR_SSD1306_COLUMNS - 1 ? 0 : p->col + 1;
			break;
	}
	p->s.seq++;
}

/* Number of argument bytes a command takes, 0 for none. */
static int
cmd_args(uint8_t c)
{
	switch (c) {
		case 0x81: case 0x20: case 0xA8: case 0xD3: case 0xD5: case 0xD9:
		case 0xDA: case 0xDB: case 0x8D:
			return 1;
		case 0x21: case 0x22: case 0xA3:
			return 2;
		case 0x29: case 0x2A:
			return 5;
		case 0x26: case 0x27:
			return 6;
	}
	return 0;
}

static void
finish_cmd(avr_ssd1306_t * p)
{
	uint8_t a = p->args[0];
	switch (p->cmd) {
		case 0x81: p->s.contrast = a; break;
		case 0x20:
			if ((a & 3) != 3)			/* 11b is invalid: ignored */
				p->s.addr_mode = a & 3;
			break;
		case 0x21:
			p->col_start = p->col = a & 127;
			p->col_end = p->args[1] & 127;
			break;
		case 0x22:
			p->page_start = p->page = a & 7;
			p->page_end = p->args[1] & 7;
			break;
		default: break;				/* accepted, not modelled */
	}
	p->s.seq++;
}

static void
write_command(avr_ssd1306_t * p, uint8_t b)
{
	if (p->args_left) {
		p->args[p->args_n++] = b;
		if (--p->args_left == 0)
			finish_cmd(p);
		return;
	}
	if (b >= 0xB0 && b <= 0xB7) {			/* page start, page mode */
		p->page = b & 7;
	} else if (b <= 0x0F) {				/* column low nibble */
		if (p->s.addr_mode == AVR_SSD1306_ADDR_PAGE)
			p->col = (p->col & 0xF0) | (b & 0x0F);
	} else if (b >= 0x10 && b <= 0x1F) {		/* column high nibble */
		if (p->s.addr_mode == AVR_SSD1306_ADDR_PAGE)
			p->col = (p->col & 0x0F) | ((b & 0x0F) << 4);
	} else if (b >= 0x40 && b <= 0x7F) {
		p->s.start_line = b & 0x3F;
	} else switch (b) {
		case 0xA4: p->s.all_on = 0; break;
		case 0xA5: p->s.all_on = 1; break;
		case 0xA6: p->s.inverse = 0; break;
		case 0xA7: p->s.inverse = 1; break;
		case 0xAE: p->s.on = 0; break;
		case 0xAF: p->s.on = 1; break;
		case 0xA0: p->s.remap = 0; break;
		case 0xA1: p->s.remap = 1; break;
		case 0xC0: p->s.com_flip = 0; break;
		case 0xC8: p->s.com_flip = 1; break;
		default: {
			int n = cmd_args(b);
			if (n) {
				p->cmd = b;
				p->args_n = 0;
				p->args_left = n;
			}
			/* else: NOP / unknown / 2Eh / 2Fh scroll on-off: ignored */
		}
	}
	p->s.seq++;
}

static void
byte_in(avr_ssd1306_t * p, uint8_t b)
{
	if (p->dc)
		write_data(p, b);
	else
		write_command(p, b);
}

static void
spi_hook(struct avr_irq_t * irq, uint32_t value, void * param)
{
	avr_ssd1306_t * p = param;
	if (!p->cs_high)
		byte_in(p, value & 0xFF);
}

static void
cs_hook(struct avr_irq_t * irq, uint32_t value, void * param)
{
	((avr_ssd1306_t *)param)->cs_high = value & 1;
}

static void
dc_hook(struct avr_irq_t * irq, uint32_t value, void * param)
{
	((avr_ssd1306_t *)param)->dc = value & 1;
}

static void
res_hook(struct avr_irq_t * irq, uint32_t value, void * param)
{
	if (irq->value && !(value & 1))		/* falling edge of RES# */
		reset(param);
}

static void
twi_hook(struct avr_irq_t * irq, uint32_t value, void * param)
{
	avr_ssd1306_t * p = param;
	avr_twi_msg_irq_t v;
	v.u.v = value;
	uint8_t m = v.u.twi.msg;

	if (m & TWI_COND_STOP)
		p->selected = 0;
	if (m & TWI_COND_START) {
		p->selected = 0;
		if ((v.u.twi.addr >> 1) == p->addr7) {
			p->selected = 1;
			p->peer = v.u.twi.addr;
			p->expect_ctrl = 1;
			p->co = 0;
			p->dc = 0;
			avr_raise_irq(p->irq + IRQ_TWI_IN,
					avr_twi_irq_msg(TWI_COND_ACK, p->peer, 1));
		}
	}
	if (!p->selected)
		return;
	if (m & TWI_COND_WRITE) {
		avr_raise_irq(p->irq + IRQ_TWI_IN,
				avr_twi_irq_msg(TWI_COND_ACK, p->peer, 1));
		if (p->expect_ctrl) {			/* Co, D/C#, then six zero bits */
			p->co = v.u.twi.data >> 7 & 1;
			p->dc = v.u.twi.data >> 6 & 1;
			p->expect_ctrl = 0;
		} else {
			byte_in(p, v.u.twi.data);
			if (p->co)				/* Co=1: control byte before each data byte */
				p->expect_ctrl = 1;
		}
	}
	if (m & TWI_COND_READ)				/* no read-back on serial interfaces */
		avr_raise_irq(p->irq + IRQ_TWI_IN,
				avr_twi_irq_msg(TWI_COND_READ, p->peer, 0));
}

static const char * irq_names[IRQ_COUNT] = {
	[IRQ_SPI_IN] = "=ssd1306.SDIN", [IRQ_CS] = "<ssd1306.CS",
	[IRQ_DC] = "<ssd1306.DC", [IRQ_RES] = "<ssd1306.RES",
	[IRQ_TWI_OUT] = "32<ssd1306.TWI.out", [IRQ_TWI_IN] = "8>ssd1306.TWI.in",
};

static avr_ssd1306_t *
alloc(avr_t * avr)
{
	avr_ssd1306_t * p = calloc(1, sizeof(*p));
	if (!p)
		return NULL;
	p->avr = avr;
	p->irq = avr_alloc_irq(&avr->irq_pool, 0, IRQ_COUNT, irq_names);
	if (!p->irq) {
		free(p);
		return NULL;
	}
	reset(p);
	p->s.seq = 1;
	p->dc = 0;
	avr_irq_register_notify(p->irq + IRQ_SPI_IN, spi_hook, p);
	avr_irq_register_notify(p->irq + IRQ_CS, cs_hook, p);
	avr_irq_register_notify(p->irq + IRQ_DC, dc_hook, p);
	avr_irq_register_notify(p->irq + IRQ_RES, res_hook, p);
	avr_irq_register_notify(p->irq + IRQ_TWI_OUT, twi_hook, p);
	return p;
}

static int
connect_pin(avr_ssd1306_t * p, int which, char port, uint8_t pin)
{
	avr_irq_t * src = avr_io_getirq(p->avr, AVR_IOCTL_IOPORT_GETIRQ(port), pin);
	if (!src)
		return -1;
	avr_connect_irq(src, p->irq + which);
	p->wired[which] = 1;
	return 0;
}

avr_ssd1306_t *
avr_ssd1306_new_twi(avr_t * avr, char bus, uint8_t addr7, char res_port, uint8_t res_pin)
{
	avr_irq_t * out = avr_io_getirq(avr, AVR_IOCTL_TWI_GETIRQ(bus), TWI_IRQ_OUTPUT);
	avr_irq_t * in = avr_io_getirq(avr, AVR_IOCTL_TWI_GETIRQ(bus), TWI_IRQ_INPUT);
	if (!out || !in)
		return NULL;
	avr_ssd1306_t * p = alloc(avr);
	if (!p)
		return NULL;
	p->is_twi = 1;
	p->addr7 = addr7;
	avr_connect_irq(out, p->irq + IRQ_TWI_OUT);
	avr_connect_irq(p->irq + IRQ_TWI_IN, in);
	p->wired[IRQ_TWI_OUT] = p->wired[IRQ_TWI_IN] = 1;
	if (res_port && connect_pin(p, IRQ_RES, res_port, res_pin)) {
		avr_ssd1306_free(p);
		return NULL;
	}
	return p;
}

avr_ssd1306_t *
avr_ssd1306_new_spi(avr_t * avr, char bus, char cs_port, uint8_t cs_pin,
		char dc_port, uint8_t dc_pin, char res_port, uint8_t res_pin)
{
	avr_irq_t * out = avr_io_getirq(avr, AVR_IOCTL_SPI_GETIRQ(bus), SPI_IRQ_OUTPUT);
	if (!out)
		return NULL;
	avr_ssd1306_t * p = alloc(avr);
	if (!p)
		return NULL;
	avr_connect_irq(out, p->irq + IRQ_SPI_IN);
	p->wired[IRQ_SPI_IN] = 1;
	if ((cs_port && connect_pin(p, IRQ_CS, cs_port, cs_pin)) ||
			connect_pin(p, IRQ_DC, dc_port, dc_pin) ||
			(res_port && connect_pin(p, IRQ_RES, res_port, res_pin))) {
		avr_ssd1306_free(p);
		return NULL;
	}
	return p;
}

void
avr_ssd1306_snapshot(const avr_ssd1306_t * p, avr_ssd1306_state_t * out)
{
	*out = p->s;
}

void
avr_ssd1306_free(avr_ssd1306_t * p)
{
	if (!p)
		return;
	avr_irq_unregister_notify(p->irq + IRQ_SPI_IN, spi_hook, p);
	avr_irq_unregister_notify(p->irq + IRQ_CS, cs_hook, p);
	avr_irq_unregister_notify(p->irq + IRQ_DC, dc_hook, p);
	avr_irq_unregister_notify(p->irq + IRQ_RES, res_hook, p);
	avr_irq_unregister_notify(p->irq + IRQ_TWI_OUT, twi_hook, p);
	free(p);
}
