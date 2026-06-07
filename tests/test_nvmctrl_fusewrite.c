#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sim_avr.h"
#include "sim_io.h"

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

static avr_t *make_modern(const char *name)
{
	avr_t *m = avr_make_mcu_by_name(name);
	if (!m) {
		printf("cannot make %s core\n", name);
		exit(2);
	}
	m->log = LOG_ERROR;
	avr_init(m);
	memset(m->flash, 0, m->flashend + 1);
	return m;
}

static void nvm_run(avr_t *m)
{
	for (int i = 0; i < 200; i++)
		avr_run(m);
}

int main(void)
{
	const avr_io_addr_t NV = 0x1000;
	enum { CTRLA = 0x00, STATUS = 0x02, DATAL = 0x06, DATAH = 0x07,
		   ADDRL = 0x08, ADDRH = 0x09 };
	enum { CMD_FUSEWRITE = 7 };
	enum { FBUSY = 0x01, WRERROR = 0x04 };

	printf("== modern NVMCTRL FUSEWRITE ==\n");
	{
		avr_t *m = make_modern("attiny3217");

		cpu_write(m, NV + ADDRL, 5);
		cpu_write(m, NV + ADDRH, 0);
		cpu_write(m, NV + DATAL, 0xa5);
		cpu_write(m, NV + DATAH, 0x5a);
		m->arch.ccp_window = 0;
		cpu_write(m, NV + CTRLA, CMD_FUSEWRITE);
		check("FUSEWRITE ignored without CCP", m->fuse[5], 0x00);

		avr_ccp_write(m, AVR_CCP_IOREG);
		cpu_write(m, NV + CTRLA, CMD_FUSEWRITE);
		check("FUSEWRITE updates addressed fuse byte", m->fuse[5], 0xa5);
		check("FUSEWRITE sets flash busy phase", !!(m->data[NV + STATUS] & FBUSY), 1);
		nvm_run(m);
		check("FUSEWRITE busy clears after delay", !!(m->data[NV + STATUS] & FBUSY), 0);
		check("FUSEWRITE does not set WRERROR on valid index",
			  !!(m->data[NV + STATUS] & WRERROR), 0);
		check("DATAH left untouched/readable", m->data[NV + DATAH], 0x5a);

		cpu_write(m, NV + ADDRL, 10);
		cpu_write(m, NV + ADDRH, 0);
		cpu_write(m, NV + DATAL, 0x3c);
		avr_ccp_write(m, AVR_CCP_IOREG);
		cpu_write(m, NV + CTRLA, CMD_FUSEWRITE);
		check("out-of-range fuse index sets WRERROR",
			  !!(m->data[NV + STATUS] & WRERROR), 1);
		check("out-of-range fuse index leaves prior fuse value intact", m->fuse[5], 0xa5);
		check("failed FUSEWRITE does not start busy phase",
			  !!(m->data[NV + STATUS] & FBUSY), 0);

		cpu_write(m, NV + ADDRL, 1);
		cpu_write(m, NV + DATAL, 0x44);
		avr_ccp_write(m, AVR_CCP_IOREG);
		cpu_write(m, NV + CTRLA, CMD_FUSEWRITE);
		check("next valid FUSEWRITE clears prior WRERROR",
			  !!(m->data[NV + STATUS] & WRERROR), 0);
		check("valid FUSEWRITE can update BOD fuse", m->fuse[1], 0x44);
	}

	if (failures) {
		printf("FAILURES=%d\n", failures);
		return 1;
	}
	printf("PASS\n");
	return 0;
}
