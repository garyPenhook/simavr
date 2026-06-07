#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sim_avr.h"
#include "sim_io.h"
#include "avr_rstctrl.h"

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

static avr_rstctrl_t *find_rstctrl(avr_t *avr)
{
	return (avr_rstctrl_t *)avr->io[AVR_DATA_TO_IO(0x0040)].w.param;
}

int main(void)
{
	const avr_io_addr_t R = 0x0040;
	const avr_io_addr_t W = 0x0100;
	enum { RSTFR = 0x00 };
	enum { CTRLA = 0x00 };
	enum { PORF = 0x01, EXTRF = 0x04, WDRF = 0x08, UPDIRF = 0x20 };
	enum { PERIOD_8CLK = 0x01 };

	printf("== modern RSTCTRL causes ==\n");
	{
		avr_t *m = make_modern("attiny3217", 0x2000);
		avr_rstctrl_t *rst = find_rstctrl(m);
		if (!rst) {
			printf("cannot find rstctrl io\n");
			return 2;
		}
		m->flash[0] = 0xff; m->flash[1] = 0xcf;	/* RJMP .-2 self-loop */
		m->pc = 0;

		cpu_write(m, R + RSTFR, PORF);
		avr_rstctrl_request_reset(rst, AVR_RSTCTRL_EXTRF);
		avr_run(m);
		check("EXTRF set after requested external reset",
			  !!(m->data[R + RSTFR] & EXTRF), 1);
		check("PORF not re-set on external reset",
			  !!(m->data[R + RSTFR] & PORF), 0);
	}

	{
		avr_t *m = make_modern("attiny3217", 0x2000);
		avr_rstctrl_t *rst = find_rstctrl(m);
		if (!rst) {
			printf("cannot find rstctrl io\n");
			return 2;
		}
		m->flash[0] = 0xff; m->flash[1] = 0xcf;
		m->pc = 0;

		cpu_write(m, R + RSTFR, PORF);
		avr_rstctrl_request_reset(rst, AVR_RSTCTRL_UPDIRF);
		avr_run(m);
		check("UPDIRF set after requested UPDI reset",
			  !!(m->data[R + RSTFR] & UPDIRF), 1);
		check("PORF not re-set on UPDI reset",
			  !!(m->data[R + RSTFR] & PORF), 0);
	}

	{
		avr_t *m = make_modern("attiny3217", 0x2000);
		m->flash[0] = 0xff; m->flash[1] = 0xcf;
		m->pc = 0;

		cpu_write(m, R + RSTFR, PORF);
		avr_ccp_write(m, AVR_CCP_IOREG);
		cpu_write(m, W + CTRLA, PERIOD_8CLK);
		for (int i = 0; i < 60000 && !(m->data[R + RSTFR] & WDRF); i++)
			avr_run(m);
		check("WDRF set after watchdog timeout", !!(m->data[R + RSTFR] & WDRF), 1);
		check("PORF not re-set on watchdog reset", !!(m->data[R + RSTFR] & PORF), 0);
	}

	if (failures) {
		printf("FAILURES=%d\n", failures);
		return 1;
	}
	printf("PASS\n");
	return 0;
}
