#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sim_avr.h"
#include "avr_crcscan.h"

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

static avr_t *make_modern(const char *name)
{
	avr_t *m = avr_make_mcu_by_name(name);
	if (!m) {
		printf("cannot make %s core\n", name);
		exit(2);
	}
	m->log = LOG_ERROR;
	avr_init(m);
	memset(m->flash, 0xff, m->flashend + 1);
	return m;
}

static void install_loop_and_crc(avr_t *avr)
{
	uint16_t crc;
	size_t flash_size = avr->flashend + 1;

	avr->flash[0] = 0xff;
	avr->flash[1] = 0xcf;	/* RJMP .-2 self-loop */
	crc = avr_crcscan_crc16(avr->flash, flash_size - 2);
	avr->flash[flash_size - 2] = crc >> 8;
	avr->flash[flash_size - 1] = crc & 0xff;
}

int main(void)
{
	const avr_io_addr_t C = 0x0120;
	enum { STATUS = 0x02 };
	enum { OK = 0x02 };
	enum { CRCSRC_FLASH = 0x00, CRCSRC_NOCRC = 0xc0 };

	printf("== modern CRCSCAN boot-time scan ==\n");
	{
		avr_t *m = make_modern("attiny3217");
		install_loop_and_crc(m);
		m->fuse[5] = CRCSRC_FLASH | 0x01;
		avr_reset(m);
		check("tiny boot CRC pass keeps CPU running", m->state, cpu_Running);
		check("tiny boot CRC sets OK", !!(m->data[C + STATUS] & OK), 1);
	}

	{
		avr_t *m = make_modern("atmega4809");
		install_loop_and_crc(m);
		m->fuse[5] = CRCSRC_FLASH | 0x01;
		avr_reset(m);
		check("mega boot CRC pass keeps CPU running", m->state, cpu_Running);
		check("mega boot CRC sets OK", !!(m->data[C + STATUS] & OK), 1);
	}

	{
		avr_t *m = make_modern("attiny3217");
		m->flash[0] = 0xff;
		m->flash[1] = 0xcf;
		m->fuse[5] = CRCSRC_FLASH | 0x01;
		m->fuse[7] = 0;
		m->fuse[8] = 0;
		avr_reset(m);
		check("boot CRC failure stops CPU before execution", m->state, cpu_Stopped);
		check("boot CRC failure clears OK", !!(m->data[C + STATUS] & OK), 0);
		check("boot CRC failure leaves PC at reset vector", m->pc, m->reset_pc);
	}

	{
		avr_t *m = make_modern("attiny3217");
		m->flash[0] = 0xff;
		m->flash[1] = 0xcf;
		m->fuse[5] = CRCSRC_NOCRC;
		avr_reset(m);
		check("NOCRC fuse leaves startup running", m->state, cpu_Running);
	}

	if (failures) {
		printf("FAILURES=%d\n", failures);
		return 1;
	}
	printf("PASS\n");
	return 0;
}
