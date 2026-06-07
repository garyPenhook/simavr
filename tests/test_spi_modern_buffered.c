#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sim_avr.h"
#include "sim_io.h"
#include "sim_irq.h"
#include "avr_spi.h"

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

typedef struct spi_trace_t {
	avr_t *avr;
	uint8_t out[8];
	uint8_t count;
} spi_trace_t;

static void spi_echo_trace(struct avr_irq_t *irq, uint32_t value, void *param)
{
	spi_trace_t *t = (spi_trace_t *)param;
	(void)irq;
	if (t->count < sizeof(t->out))
		t->out[t->count++] = value & 0xff;
	avr_raise_irq(avr_io_getirq(t->avr, AVR_IOCTL_SPI_GETIRQ('0'),
								 SPI_IRQ_INPUT), (value ^ 0xff) & 0xff);
}

static void spi_capture_trace(struct avr_irq_t *irq, uint32_t value, void *param)
{
	spi_trace_t *t = (spi_trace_t *)param;
	(void)irq;
	if (t->count < sizeof(t->out))
		t->out[t->count++] = value & 0xff;
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

int main(void)
{
	const avr_io_addr_t S = 0x0820; /* tiny SPI0 */
	enum {
		ENABLE = 0x01, MASTER = 0x20,
		BUFEN = 0x80,
		RXCIE = 0x80, TXCIE = 0x40, DREIE = 0x20,
		RXCIF = 0x80, TXCIF = 0x40, DREIF = 0x20, BUFOVF = 0x01
	};

	printf("== modern SPI buffered host ==\n");
	{
		avr_t *m = make_modern("attiny3217", 0x2000);
		spi_trace_t tr = { .avr = m };
		avr_irq_register_notify(avr_io_getirq(m, AVR_IOCTL_SPI_GETIRQ('0'),
											  SPI_IRQ_OUTPUT),
								spi_echo_trace, &tr);

		cpu_write(m, S + 0x01, BUFEN);
		cpu_write(m, S + 0x02, RXCIE | TXCIE | DREIE);
		cpu_write(m, S + 0x00, ENABLE | MASTER);
		check("buffered reset marks DREIF", !!(m->data[S + 0x03] & DREIF), 1);

		cpu_write(m, S + 0x04, 0x11);
		check("first write leaves DREIF high", !!(m->data[S + 0x03] & DREIF), 1);
		cpu_write(m, S + 0x04, 0x22);
		check("second write fills TX buffer", !!(m->data[S + 0x03] & DREIF), 0);
		cpu_write(m, S + 0x04, 0x33); /* dropped: tx buffer already full */

		int first_done = 0;
		for (int i = 0; i < 200; i++) {
			avr_run(m);
			if (tr.count >= 1) { first_done = 1; break; }
		}
		check("first buffered byte shifted out", first_done, 1);
		check("first MOSI byte = 0x11", tr.out[0], 0x11);
		check("TXCIF not set while second byte queued", !!(m->data[S + 0x03] & TXCIF), 0);
		check("DREIF high after queue drains", !!(m->data[S + 0x03] & DREIF), 1);

		int second_done = 0;
		for (int i = 0; i < 200; i++) {
			avr_run(m);
			if (tr.count >= 2) { second_done = 1; break; }
		}
		check("second buffered byte shifted out", second_done, 1);
		check("second MOSI byte = 0x22", tr.out[1], 0x22);
		check("TXCIF set after final byte", !!(m->data[S + 0x03] & TXCIF), 1);
		check("RXCIF set with unread data", !!(m->data[S + 0x03] & RXCIF), 1);
		check("buffered SPI interrupt pending", avr_has_pending_interrupts(m), 1);

		check("RX FIFO byte 1", cpu_read(m, S + 0x04), 0xee);
		check("RXCIF still set (1 byte left)", !!(m->data[S + 0x03] & RXCIF), 1);
		check("RX FIFO byte 2", cpu_read(m, S + 0x04), 0xdd);
		check("RXCIF clears when drained", !!(m->data[S + 0x03] & RXCIF), 0);
	}

	printf("== modern SPI buffered host overflow ==\n");
	{
		avr_t *m = make_modern("attiny3217", 0x2000);
		spi_trace_t tr = { .avr = m };
		avr_irq_register_notify(avr_io_getirq(m, AVR_IOCTL_SPI_GETIRQ('0'),
											  SPI_IRQ_OUTPUT),
								spi_echo_trace, &tr);

		cpu_write(m, S + 0x01, BUFEN);
		cpu_write(m, S + 0x00, ENABLE | MASTER);
		cpu_write(m, S + 0x04, 0x10);
		cpu_write(m, S + 0x04, 0x20);
		for (int i = 0; i < 200 && tr.count < 1; i++) avr_run(m);
		cpu_write(m, S + 0x04, 0x30);
		for (int i = 0; i < 400 && tr.count < 3; i++) avr_run(m);
		check("three host transfers completed", tr.count, 3);
		check("BUFOVF set on 3rd unread RX byte", !!(m->data[S + 0x03] & BUFOVF), 1);
		check("overflow read returns oldest byte", cpu_read(m, S + 0x04), 0xef);
		check("read clears BUFOVF", !!(m->data[S + 0x03] & BUFOVF), 0);
		check("overflow read byte 2", cpu_read(m, S + 0x04), 0xdf);
	}

	printf("== modern SPI buffered client ==\n");
	{
		avr_t *m = make_modern("attiny3217", 0x2000);
		spi_trace_t tr = { .avr = m };
		avr_irq_register_notify(avr_io_getirq(m, AVR_IOCTL_SPI_GETIRQ('0'),
											  SPI_IRQ_OUTPUT),
								spi_capture_trace, &tr);

		cpu_write(m, S + 0x01, BUFEN);
		cpu_write(m, S + 0x00, ENABLE);
		cpu_write(m, S + 0x04, 0x3c);
		cpu_write(m, S + 0x04, 0x5a);
		avr_raise_irq(avr_io_getirq(m, AVR_IOCTL_SPI_GETIRQ('0'), SPI_IRQ_INPUT), 0x11);
		avr_raise_irq(avr_io_getirq(m, AVR_IOCTL_SPI_GETIRQ('0'), SPI_IRQ_INPUT), 0x22);

		check("client first echoed byte", tr.out[0], 0x3c);
		check("client second echoed queued byte", tr.out[1], 0x5a);
		check("client RX byte 1", cpu_read(m, S + 0x04), 0x11);
		check("client RX byte 2", cpu_read(m, S + 0x04), 0x22);
		check("client RXCIF clears when drained", !!(m->data[S + 0x03] & RXCIF), 0);
		check("client TXCIF set when queue empty", !!(m->data[S + 0x03] & TXCIF), 1);
	}

	if (failures) {
		printf("FAILURES=%d\n", failures);
		return 1;
	}
	printf("PASS\n");
	return 0;
}
