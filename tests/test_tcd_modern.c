#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sim_avr.h"
#include "sim_io.h"
#include "sim_irq.h"
#include "avr_tcd.h"

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
	*(uint8_t *)param = (uint8_t)value;
}

int main(void)
{
	const avr_io_addr_t T = 0x0a80;
	enum { ENABLE = 0x01, OVF = 0x01, CMPAEN = 0x10, CMPBEN = 0x20 };
	enum { ONERAMP = 0x00, TWORAMP = 0x01, FOURRAMP = 0x02, DUAL = 0x03 };

	printf("== modern TCD waveform modes ==\n");
	{
		avr_t *m = make_modern("attiny3217", 0x2000);
		uint8_t woa = 0xff, wob = 0xff;
		avr_irq_register_notify(
			avr_io_getirq(m, AVR_IOCTL_TCD_GETIRQ('0'), AVR_TCD_IRQ_WOA), rec_irq, &woa);
		avr_irq_register_notify(
			avr_io_getirq(m, AVR_IOCTL_TCD_GETIRQ('0'), AVR_TCD_IRQ_WOB), rec_irq, &wob);

		cpu_write(m, T + TCDR_CTRLB, TWORAMP);
		cpu_write(m, T + TCDR_CMPASETL, 3);
		cpu_write(m, T + TCDR_CMPACLRL, 9);
		cpu_write(m, T + TCDR_CMPBSETL, 4);
		cpu_write(m, T + TCDR_CMPBCLRL, 7);
		cpu_write(m, T + TCDR_INTCTRL, OVF);
		cpu_write(m, T + TCDR_FAULTCTRL, CMPAEN | CMPBEN);
		long t0 = (long)m->cycle;
		cpu_write(m, T + TCDR_CTRLA, ENABLE);

		while ((long)m->cycle - t0 < 5) avr_run(m);
		check("two-ramp WOA high in first ramp", woa, 1);
		while ((long)m->cycle - t0 < 12) avr_run(m);
		check("two-ramp WOA low in second ramp", woa, 0);
		check("two-ramp WOB still low before set", wob, 0xff);
		while ((long)m->cycle - t0 < 16) avr_run(m);
		check("two-ramp WOB high in second ramp", wob, 1);
		long tovf = -1;
		while ((long)m->cycle - t0 < 40 && tovf < 0) {
			avr_run(m);
			if (m->data[T + TCDR_INTFLAGS] & OVF)
				tovf = (long)m->cycle;
		}
		check("two-ramp period near 18 cycles", (tovf - t0) >= 16 && (tovf - t0) <= 20, 1);
	}

	{
		avr_t *m = make_modern("attiny3217", 0x2000);
		uint8_t woa = 0xff, wob = 0xff;
		avr_irq_register_notify(
			avr_io_getirq(m, AVR_IOCTL_TCD_GETIRQ('0'), AVR_TCD_IRQ_WOA), rec_irq, &woa);
		avr_irq_register_notify(
			avr_io_getirq(m, AVR_IOCTL_TCD_GETIRQ('0'), AVR_TCD_IRQ_WOB), rec_irq, &wob);

		cpu_write(m, T + TCDR_CTRLB, FOURRAMP);
		cpu_write(m, T + TCDR_CMPASETL, 2);
		cpu_write(m, T + TCDR_CMPACLRL, 3);
		cpu_write(m, T + TCDR_CMPBSETL, 4);
		cpu_write(m, T + TCDR_CMPBCLRL, 5);
		cpu_write(m, T + TCDR_FAULTCTRL, CMPAEN | CMPBEN);
		long t0 = (long)m->cycle;
		cpu_write(m, T + TCDR_CTRLA, ENABLE);

		while ((long)m->cycle - t0 < 2) avr_run(m);
		check("four-ramp stays low in DTA", woa, 0xff);
		while ((long)m->cycle - t0 < 4) avr_run(m);
		check("four-ramp WOA high in OTA", woa, 1);
		while ((long)m->cycle - t0 < 8) avr_run(m);
		check("four-ramp WOA low again in DTB", woa, 0);
		while ((long)m->cycle - t0 < 13) avr_run(m);
		check("four-ramp WOB high in OTB", wob, 1);
	}

	{
		avr_t *m = make_modern("attiny3217", 0x2000);
		uint8_t woa = 0xff, wob = 0xff;
		avr_irq_register_notify(
			avr_io_getirq(m, AVR_IOCTL_TCD_GETIRQ('0'), AVR_TCD_IRQ_WOA), rec_irq, &woa);
		avr_irq_register_notify(
			avr_io_getirq(m, AVR_IOCTL_TCD_GETIRQ('0'), AVR_TCD_IRQ_WOB), rec_irq, &wob);

		cpu_write(m, T + TCDR_CTRLB, DUAL);
		cpu_write(m, T + TCDR_CMPASETL, 3);
		cpu_write(m, T + TCDR_CMPBSETL, 7);
		cpu_write(m, T + TCDR_CMPBCLRL, 9);
		cpu_write(m, T + TCDR_INTCTRL, OVF);
		cpu_write(m, T + TCDR_FAULTCTRL, CMPAEN | CMPBEN);
		long t0 = (long)m->cycle;
		cpu_write(m, T + TCDR_CTRLA, ENABLE);

		check("dual-slope initial WOA low", woa, 0xff);
		check("dual-slope initial WOB low", wob, 0xff);
		while ((long)m->cycle - t0 < 3) avr_run(m);
		check("dual-slope WOB high on down-count match", wob, 1);
		while ((long)m->cycle - t0 < 14) avr_run(m);
		check("dual-slope WOA high on up-count match", woa, 1);
		check("dual-slope outputs can overlap", wob, 1);
		while ((long)m->cycle - t0 < 18) avr_run(m);
		check("dual-slope WOB clears on up-count match", wob, 0);
		long tovf = -1;
		while ((long)m->cycle - t0 < 30 && tovf < 0) {
			avr_run(m);
			if (m->data[T + TCDR_INTFLAGS] & OVF)
				tovf = (long)m->cycle;
		}
		check("dual-slope period near 20 cycles", (tovf - t0) >= 18 && (tovf - t0) <= 22, 1);
	}

	{
		avr_t *m = make_modern("attiny3217", 0x2000);
		uint8_t woa = 0xff, wob = 0xff;
		avr_irq_register_notify(
			avr_io_getirq(m, AVR_IOCTL_TCD_GETIRQ('0'), AVR_TCD_IRQ_WOA), rec_irq, &woa);
		avr_irq_register_notify(
			avr_io_getirq(m, AVR_IOCTL_TCD_GETIRQ('0'), AVR_TCD_IRQ_WOB), rec_irq, &wob);

		cpu_write(m, T + TCDR_CTRLB, ONERAMP);
		cpu_write(m, T + TCDR_CMPASETL, 2);
		cpu_write(m, T + TCDR_CMPACLRL, 5);
		cpu_write(m, T + TCDR_CMPBSETL, 6);
		cpu_write(m, T + TCDR_CMPBCLRL, 9);
		cpu_write(m, T + TCDR_FAULTCTRL, CMPAEN | CMPBEN);
		long t0 = (long)m->cycle;
		cpu_write(m, T + TCDR_CTRLA, ENABLE);

		while ((long)m->cycle - t0 < 3) avr_run(m);
		check("one-ramp WOA still works", woa, 1);
		while ((long)m->cycle - t0 < 8) avr_run(m);
		check("one-ramp WOB still works", wob, 1);
	}

	if (failures) {
		printf("FAILURES=%d\n", failures);
		return 1;
	}
	printf("PASS\n");
	return 0;
}
