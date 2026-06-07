#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sim_avr.h"
#include "sim_io.h"
#include "sim_irq.h"
#include "avr_usart_modern.h"

static int failures;

static void check(const char *what, long got, long want)
{
	if (got != want) {
		printf("  FAIL: %-40s got %ld, want %ld\n", what, got, want);
		failures++;
	} else {
		printf("  ok:   %-40s = %ld\n", what, got);
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

static avr_t *make_modern(const char *name, size_t flash_fill)
{
	avr_t *m = avr_make_mcu_by_name(name);
	if (!m) {
		printf("cannot make %s core\n", name);
		exit(2);
	}
	m->log = LOG_ERROR;
	avr_init(m);
	memset(m->flash, 0, flash_fill);
	return m;
}

static void rec_irq(struct avr_irq_t *irq, uint32_t value, void *param)
{
	(void)irq;
	*(uint32_t *)param = value;
}

int main(void)
{
	const avr_io_addr_t U = 0x0800;
	enum { RXDATAL = 0x00, TXDATAL = 0x02, STATUS = 0x04, CTRLA = 0x05,
		   CTRLB = 0x06, CTRLC = 0x07, BAUDL = 0x08, BAUDH = 0x09 };
	enum { RXCIF = 0x80, TXCIF = 0x40 };
	enum { LBME = 0x08 };
	enum { ODME = 0x08, RXEN = 0x80, TXEN = 0x40, CLK2X = 0x02 };
	enum { CMODE_SYNC = 0x40 };

	printf("== modern USART sync / one-wire ==\n");
	{
		avr_t *m = make_modern("attiny3217", 0x2000);
		uint32_t out = 0;
		uint32_t ioctl = AVR_IOCTL_UART_GETIRQ('0');
		avr_irq_register_notify(avr_io_getirq(m, ioctl, UART_IRQ_OUTPUT), rec_irq, &out);

		cpu_write(m, U + BAUDL, 64);
		cpu_write(m, U + BAUDH, 0);
		cpu_write(m, U + CTRLB, TXEN);
		long t0 = (long)m->cycle;
		cpu_write(m, U + TXDATAL, 'A');
		check("async TX still emits output byte", out, 'A');

		long async_txc = -1;
		for (int i = 0; i < 500 && async_txc < 0; i++) {
			avr_run(m);
			if (m->data[U + STATUS] & TXCIF)
				async_txc = (long)m->cycle;
		}
		check("async frame completes", async_txc > 0, 1);

		cpu_write(m, U + STATUS, TXCIF);
		cpu_write(m, U + CTRLC, CMODE_SYNC);
		t0 = (long)m->cycle;
		cpu_write(m, U + TXDATAL, 'B');
		long sync_txc = -1;
		for (int i = 0; i < 700 && sync_txc < 0; i++) {
			avr_run(m);
			if (m->data[U + STATUS] & TXCIF)
				sync_txc = (long)m->cycle;
		}
		check("sync frame completes", sync_txc > 0, 1);
		check("sync frame slower than async for same BAUD",
			  (sync_txc - t0) > (async_txc - (long)0), 1);
	}

	{
		avr_t *m = make_modern("attiny3217", 0x2000);
		uint32_t out = 0;
		uint32_t ioctl = AVR_IOCTL_UART_GETIRQ('0');
		avr_irq_register_notify(avr_io_getirq(m, ioctl, UART_IRQ_OUTPUT), rec_irq, &out);

		cpu_write(m, U + BAUDL, 32);
		cpu_write(m, U + BAUDH, 0);
		cpu_write(m, U + CTRLA, LBME);
		cpu_write(m, U + CTRLB, TXEN | RXEN | ODME);
		cpu_write(m, U + TXDATAL, 'Z');
		check("one-wire TX still emits output byte", out, 'Z');
		check("one-wire does not RX immediately", !!(m->data[U + STATUS] & RXCIF), 0);

		for (int i = 0; i < 400 && !(m->data[U + STATUS] & RXCIF); i++)
			avr_run(m);
		check("one-wire loopback raises RXCIF", !!(m->data[U + STATUS] & RXCIF), 1);
		check("one-wire loopback returns TX byte", cpu_read(m, U + RXDATAL), 'Z');
		check("one-wire drain clears RXCIF", !!(m->data[U + STATUS] & RXCIF), 0);
	}

	{
		avr_t *m = make_modern("attiny3217", 0x2000);
		avr_irq_t *in = avr_io_getirq(m, AVR_IOCTL_UART_GETIRQ('0'), UART_IRQ_INPUT);

		cpu_write(m, U + BAUDL, 16);
		cpu_write(m, U + BAUDH, 0);
		cpu_write(m, U + CTRLA, LBME);
		cpu_write(m, U + CTRLB, RXEN | TXEN | CLK2X);
		avr_raise_irq(in, 'Q');
		check("one-wire external shared-line input still received",
			  cpu_read(m, U + RXDATAL), 'Q');
	}

	if (failures) {
		printf("FAILURES=%d\n", failures);
		return 1;
	}
	printf("PASS\n");
	return 0;
}
