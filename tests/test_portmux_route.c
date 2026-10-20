/*
 * PORTMUX routing + PORT override layer, host-only (no avr-gcc).
 *
 * Registers a function on the ATtiny3217 PORTMUX by hand (the layer is generic:
 * a routing field picks the pin, the peripheral supplies active/value) and
 * checks that the pin takes the peripheral's level, that switching PORTMUX
 * moves it and releases the old pin, and that DIR/OUT are never altered.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sim_avr.h"
#include "sim_io.h"
#include "avr_portmux.h"
#include "avr_tca.h"
#include "avr_usart_modern.h"

static int failures;

static void check(const char *what, long got, long want)
{
	if (got != want) {
		printf("  FAIL: %-44s got %ld, want %ld\n", what, got, want);
		failures++;
	} else {
		printf("  ok:   %-44s = %ld\n", what, got);
	}
}

static void cpu_write(avr_t *avr, uint16_t addr, uint8_t v)
{
	avr_io_addr_t io = AVR_DATA_TO_IO(addr);
	if (avr->io[io].w.c)
		avr->io[io].w.c(avr, addr, v, avr->io[io].w.param);
	else
		avr_core_watch_write(avr, addr, v);
}

static uint8_t cpu_read(avr_t *avr, uint16_t addr)
{
	avr_io_addr_t io = AVR_DATA_TO_IO(addr);
	if (avr->io[io].r.c)
		return avr->io[io].r.c(avr, addr, avr->io[io].r.param);
	return avr->data[addr];
}

/* Level last raised on a PORT pin output IRQ. */
static void rec(struct avr_irq_t *irq, uint32_t v, void *param)
{
	(void)irq;
	*(int *)param = v & 1;
}

enum { PORTA = 0x0400, PORTB = 0x0420, PORTC = 0x0440 };
enum { DIR = 0, OUT = 4, IN = 8 };

int main(void)
{
	avr_t *avr = avr_make_mcu_by_name("attiny3217");
	if (!avr) { printf("cannot make core\n"); return 2; }
	avr->log = LOG_ERROR;
	avr_init(avr);
	avr_reset(avr);

	avr_portmux_t *pm = (avr_portmux_t *)avr->io[AVR_DATA_TO_IO(0x0200)].w.param;

	/* CTRLC bit 0 selects: 0 -> PB0, 1 -> PB3 (a TCA0 WO0-like pair). */
	const avr_portmux_pin_t pins[2] = { { 'B', 0 }, { 'B', 3 } };
	int fn = avr_portmux_add_func(pm, 2, 0x01, 0, pins, 2, 0);
	int fnf = avr_portmux_add_func(pm, 1, 0x01, 0, (avr_portmux_pin_t[]){ { 'A', 1 }, { 'A', 2 } },
								   2, 1);
	check("function handles", fn >= 0 && fnf >= 0 && fn != fnf, 1);

	int pb0 = -1, pb3 = -1, pa1 = -1;
	avr_irq_register_notify(avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ('B'), 0), rec, &pb0);
	avr_irq_register_notify(avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ('B'), 3), rec, &pb3);
	avr_irq_register_notify(avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ('A'), 1), rec, &pa1);

	printf("== override with DIR still deciding ==\n");
	cpu_write(avr, PORTB + DIR, 0x09);			/* PB0, PB3 outputs */
	cpu_write(avr, PORTB + OUT, 0x00);
	check("PB0 follows OUT before the peripheral", pb0, 0);
	avr_portmux_func_set(pm, fn, 1, 1);
	check("PB0 driven high by peripheral", pb0, 1);
	check("PORTB.OUT unchanged", avr->data[PORTB + OUT], 0x00);
	check("PORTB.IN shows the peripheral level", cpu_read(avr, PORTB + IN) & 1, 1);
	cpu_write(avr, PORTB + OUT, 0x00);
	check("CPU OUT write cannot beat the override", pb0, 1);
	avr_portmux_func_set(pm, fn, 1, 0);
	check("peripheral drives low", pb0, 0);

	printf("== PORTMUX moves the function ==\n");
	avr_portmux_func_set(pm, fn, 1, 1);
	cpu_write(avr, 0x0200 + 2, 0x01);
	check("old pin PB0 released (back to OUT=0)", pb0, 0);
	check("new pin PB3 driven high", pb3, 1);
	cpu_write(avr, 0x0200 + 2, 0x00);
	check("back to PB0", pb0, 1);
	check("PB3 released", pb3, 0);

	printf("== release ==\n");
	avr_portmux_func_set(pm, fn, 0, 0);
	check("inactive: PB0 follows OUT again", pb0, 0);

	printf("== force_output ==\n");
	cpu_write(avr, PORTA + DIR, 0x00);			/* PA1 input */
	avr_portmux_func_set(pm, fnf, 1, 1);
	check("forced-output pin driven high", pa1, 1);
	check("PORTA.DIR still reads 0", avr->data[PORTA + DIR], 0);
	avr_portmux_func_set(pm, fnf, 0, 0);

	printf("== reset ==\n");
	avr_portmux_func_set(pm, fn, 1, 1);
	avr_reset(avr);
	check("override gone after reset", cpu_read(avr, PORTB + IN) & 1, 0);

	printf("== TCA0 WO0 on its routed pin (attiny3217) ==\n");
	{
		avr_t *m = avr_make_mcu_by_name("attiny3217");
		m->log = LOG_ERROR;
		avr_init(m);
		memset(m->flash, 0, 0x2000);
		avr_reset(m);
		int b0 = -1, b3 = -1;
		avr_irq_register_notify(avr_io_getirq(m, AVR_IOCTL_IOPORT_GETIRQ('B'), 0), rec, &b0);
		avr_irq_register_notify(avr_io_getirq(m, AVR_IOCTL_IOPORT_GETIRQ('B'), 3), rec, &b3);
		const uint16_t TA = 0x0a00;
		cpu_write(m, PORTB + DIR, 0x09);	/* firmware makes PB0/PB3 outputs */
		cpu_write(m, TA + TCAR_PERL, 99); cpu_write(m, TA + TCAR_PERH, 0);
		cpu_write(m, TA + TCAR_CMP0L, 30);
		cpu_write(m, TA + TCAR_CTRLB, 0x03 | 0x10);	/* single-slope, CMP0EN */
		long t0 = (long)m->cycle;
		cpu_write(m, TA + TCAR_CTRLA, 0x01);
		check("PB0 high at BOTTOM", b0, 1);
		while ((long)m->cycle - t0 < 35) avr_run(m);
		check("PB0 low after CMP0 match", b0, 0);
		while ((long)m->cycle - t0 < 105) avr_run(m);
		check("PB0 high at the next period", b0, 1);
		check("PB3 untouched on default routing", b3, 0);
		check("PORTB.OUT untouched", m->data[PORTB + OUT], 0);

		cpu_write(m, 0x0200 + 2, 0x01);		/* PORTMUX.CTRLC: TCA00 -> PB3 */
		check("alternate: PB0 released", b0, 0);
		check("alternate: PB3 carries WO0", b3, 1);
		while ((long)m->cycle - t0 < 235) avr_run(m);
		check("alternate: PB3 low after match", b3, 0);

		cpu_write(m, TA + TCAR_CTRLB, 0x03);		/* CMP0EN off: pin released */
		check("CMP0EN clear releases PB3", b3, 0);
		cpu_write(m, PORTB + OUT, 0x08);
		check("PB3 follows OUT again", b3, 1);
	}

	printf("== USART0 TXD frame on its routed pin (attiny3217) ==\n");
	{
		avr_t *m = avr_make_mcu_by_name("attiny3217");
		m->log = LOG_ERROR;
		avr_init(m);
		memset(m->flash, 0, 0x2000);
		avr_reset(m);
		int b2 = -1, a1 = -1;
		avr_irq_register_notify(avr_io_getirq(m, AVR_IOCTL_IOPORT_GETIRQ('B'), 2), rec, &b2);
		avr_irq_register_notify(avr_io_getirq(m, AVR_IOCTL_IOPORT_GETIRQ('A'), 1), rec, &a1);
		const uint16_t U = 0x0800;
		check("CTRLC resets to 8N1 (0x03)", m->data[U + USARTR_CTRLC], 0x03);
		cpu_write(m, PORTB + DIR, 0x04);		/* TXD (PB2) is an output */
		cpu_write(m, U + USARTR_BAUDL, 400 & 0xff);
		cpu_write(m, U + USARTR_BAUDL + 1, 400 >> 8);	/* 100 cycles per bit */
		check("PB2 idle before TXEN", b2, 0);
		cpu_write(m, U + USARTR_CTRLB, 0x40);		/* TXEN */
		check("PB2 idles high once TXEN", b2, 1);
		long t0 = (long)m->cycle;
		cpu_write(m, U + USARTR_TXDATAL, 0x55);
		check("Start bit low", b2, 0);
		/* Expected: S=0, D0..D7 = 1,0,1,0,1,0,1,0, stop=1. */
		static const int want[10] = { 0, 1, 0, 1, 0, 1, 0, 1, 0, 1 };
		for (int i = 0; i < 10; i++) {
			while ((long)m->cycle - t0 < i * 100 + 50) avr_run(m);
			char what[32];
			snprintf(what, sizeof what, "bit %d", i);
			check(what, b2, want[i]);
		}
		while ((long)m->cycle - t0 < 1100) avr_run(m);
		check("idle high after the frame", b2, 1);
		check("PORTB.OUT untouched", m->data[PORTB + OUT], 0);

		cpu_write(m, 0x0200 + 1, 0x01);		/* PORTMUX.CTRLB USART0 -> PA1 */
		cpu_write(m, PORTA + DIR, 0x02);
		check("alternate: PA1 carries idle high", a1, 1);
		check("alternate: PB2 released", b2, 0);

		cpu_write(m, U + USARTR_CTRLB, 0x00);		/* TXEN off, nothing in flight */
		check("TXEN off releases PA1", a1, 0);
	}

	printf("\n%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}
