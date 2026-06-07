#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sim_avr.h"
#include "sim_io.h"
#include "sim_irq.h"
#include "avr_tcb.h"

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

static uint16_t cpu_read16(avr_t *avr, uint16_t addr)
{
	uint16_t v = cpu_read(avr, addr);
	v |= (uint16_t)cpu_read(avr, addr + 1) << 8;
	return v;
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

static void pulse_tcb_event(avr_t *avr, char name)
{
	avr_irq_t *irq = avr_io_getirq(avr, AVR_IOCTL_TCB_GETIRQ(name),
								   AVR_TCB_IRQ_EVENT_IN);
	avr_raise_irq(irq, 1);
	avr_raise_irq(irq, 0);
}

int main(void)
{
	const avr_io_addr_t TBT = 0x0a40;
	const avr_io_addr_t TBM0 = 0x0a80;
	const avr_io_addr_t TBM1 = 0x0a90;
	enum { CAPT = 0x01, RUN = 0x01 };

	printf("== modern TCB event modes ==\n");
	{
		avr_t *m = make_modern("attiny3217", 0x2000);
		cpu_write(m, TBT + TCBR_EVCTRL, 0x01);
		cpu_write(m, TBT + TCBR_CTRLB, 0x02);
		cpu_write(m, TBT + TCBR_CTRLA, 0x01);
		for (int i = 0; i < 37; i++) avr_run(m);
		pulse_tcb_event(m, '0');
		uint16_t cap = cpu_read16(m, TBT + TCBR_CCMPL);
		check("CAPT mode captures live count", cap >= 35 && cap <= 41, 1);
		check("CAPT auto-clears on CCMPL read",
			  !!(m->data[TBT + TCBR_INTFLAGS] & CAPT), 0);
	}

	{
		avr_t *m = make_modern("attiny3217", 0x2000);
		cpu_write(m, TBT + TCBR_EVCTRL, 0x01);
		cpu_write(m, TBT + TCBR_CTRLB, 0x03);
		cpu_write(m, TBT + TCBR_CTRLA, 0x01);
		for (int i = 0; i < 23; i++) avr_run(m);
		pulse_tcb_event(m, '0');
		uint16_t cap = cpu_read16(m, TBT + TCBR_CCMPL);
		check("FRQ first capture near elapsed count", cap >= 21 && cap <= 27, 1);
		for (int i = 0; i < 17; i++) avr_run(m);
		pulse_tcb_event(m, '0');
		cap = cpu_read16(m, TBT + TCBR_CCMPL);
		check("FRQ restarts after capture", cap >= 15 && cap <= 21, 1);
	}

	{
		avr_t *m = make_modern("attiny3217", 0x2000);
		cpu_write(m, TBT + TCBR_EVCTRL, 0x01);
		cpu_write(m, TBT + TCBR_CTRLB, 0x04);
		cpu_write(m, TBT + TCBR_CTRLA, 0x01);
		avr_raise_irq(avr_io_getirq(m, AVR_IOCTL_TCB_GETIRQ('0'),
									AVR_TCB_IRQ_EVENT_IN), 1);
		for (int i = 0; i < 29; i++) avr_run(m);
		avr_raise_irq(avr_io_getirq(m, AVR_IOCTL_TCB_GETIRQ('0'),
									AVR_TCB_IRQ_EVENT_IN), 0);
		uint16_t cap = cpu_read16(m, TBT + TCBR_CCMPL);
		check("PW captures high pulse width", cap >= 27 && cap <= 33, 1);
	}

	{
		avr_t *m = make_modern("attiny3217", 0x2000);
		cpu_write(m, TBT + TCBR_EVCTRL, 0x01);
		cpu_write(m, TBT + TCBR_CTRLB, 0x05);
		cpu_write(m, TBT + TCBR_CTRLA, 0x01);
		avr_raise_irq(avr_io_getirq(m, AVR_IOCTL_TCB_GETIRQ('0'),
									AVR_TCB_IRQ_EVENT_IN), 1);
		for (int i = 0; i < 19; i++) avr_run(m);
		avr_raise_irq(avr_io_getirq(m, AVR_IOCTL_TCB_GETIRQ('0'),
									AVR_TCB_IRQ_EVENT_IN), 0);
		for (int i = 0; i < 11; i++) avr_run(m);
		avr_raise_irq(avr_io_getirq(m, AVR_IOCTL_TCB_GETIRQ('0'),
									AVR_TCB_IRQ_EVENT_IN), 1);
		uint16_t cap = cpu_read16(m, TBT + TCBR_CCMPL);
		uint16_t period = cpu_read16(m, TBT + TCBR_CNTL);
		check("FRQPW width captured in CCMP", cap >= 17 && cap <= 23, 1);
		check("FRQPW period frozen in CNT", period >= 28 && period <= 34, 1);
	}

	{
		avr_t *m = make_modern("attiny3217", 0x2000);
		cpu_write(m, TBT + TCBR_CCMPL, 25);
		cpu_write(m, TBT + TCBR_CCMPH, 0);
		cpu_write(m, TBT + TCBR_EVCTRL, 0x01);
		cpu_write(m, TBT + TCBR_CTRLB, 0x01);
		cpu_write(m, TBT + TCBR_CTRLA, 0x01);
		pulse_tcb_event(m, '0');
		for (int i = 0; i < 10; i++) avr_run(m);
		pulse_tcb_event(m, '0');
		check("TIMEOUT stop edge does not set CAPT",
			  !!(m->data[TBT + TCBR_INTFLAGS] & CAPT), 0);
		check("TIMEOUT stop edge clears RUN",
			  !!(m->data[TBT + TCBR_STATUS] & RUN), 0);
		cpu_write(m, TBT + TCBR_INTFLAGS, 0xff);
		long base = (long)m->cycle;
		pulse_tcb_event(m, '0');
		long tto = -1;
		for (int i = 0; i < 80 && tto < 0; i++) {
			avr_run(m);
			if (m->data[TBT + TCBR_INTFLAGS] & CAPT)
				tto = (long)m->cycle;
		}
		check("TIMEOUT flags CAPT at TOP",
			  (tto - base) >= 25 && (tto - base) <= 31, 1);
		check("TIMEOUT freeze clears RUN", !!(m->data[TBT + TCBR_STATUS] & RUN), 0);
	}

	printf("== TCB EVSYS routing ==\n");
	{
		avr_t *m = make_modern("attiny3217", 0x2000);
		cpu_write(m, 0x018a, 0x15);	/* sim tiny EVSYS.SYNCCH0 = TCB1 */
		cpu_write(m, 0x0192, 0x01);	/* sim tiny EVSYS user0 (TCB0) = SYNCCH0 */
		cpu_write(m, TBT + TCBR_EVCTRL, 0x01);
		cpu_write(m, TBT + TCBR_CTRLB, 0x02);
		cpu_write(m, TBT + TCBR_CTRLA, 0x01);
		cpu_write(m, 0x0a50 + TCBR_EVCTRL, 0x01);
		cpu_write(m, 0x0a50 + TCBR_CTRLB, 0x02);
		cpu_write(m, 0x0a50 + TCBR_CTRLA, 0x01);
		for (int i = 0; i < 18; i++) avr_run(m);
		pulse_tcb_event(m, '1');
		check("tiny EVSYS routes TCB1 CAPT to TCB0",
			  !!(m->data[TBT + TCBR_INTFLAGS] & CAPT), 1);
	}

	{
		avr_t *m = make_modern("atmega4809", 0x3000);
		cpu_write(m, 0x0190, 0xa0);	/* sim mega EVSYS channel0 = TCB0_CAPT */
		cpu_write(m, 0x01b5, 0x01);	/* mega EVSYS.USERTCB1 (0x1B5) = channel0 */
		cpu_write(m, TBM1 + TCBR_EVCTRL, 0x01);
		cpu_write(m, TBM1 + TCBR_CTRLB, 0x02);
		cpu_write(m, TBM1 + TCBR_CTRLA, 0x01);
		cpu_write(m, TBM0 + TCBR_EVCTRL, 0x01);
		cpu_write(m, TBM0 + TCBR_CTRLB, 0x02);
		cpu_write(m, TBM0 + TCBR_CTRLA, 0x01);
		for (int i = 0; i < 18; i++) avr_run(m);
		pulse_tcb_event(m, '0');
		check("mega EVSYS routes TCB0 CAPT to TCB1",
			  !!(m->data[TBM1 + TCBR_INTFLAGS] & CAPT), 1);
	}

	if (failures) {
		printf("FAILURES=%d\n", failures);
		return 1;
	}
	printf("PASS\n");
	return 0;
}
