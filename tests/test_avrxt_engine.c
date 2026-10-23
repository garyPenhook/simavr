/*
	test_avrxt_engine.c

	Standalone unit test for the Phase 2 engine additions:
	  - AVRxt instruction timing (cycle counts)
	  - CCP (Configuration Change Protection) unlock window
	  - flash-mapped-into-data-space reads

	These are exercised by allocating a real core and toggling the modern
	architecture flags directly, then hand-assembling opcodes into flash and
	single-stepping with avr_run_one(). This avoids needing a full modern core
	(that lands in Phase 5).

	Build/run from the tests directory:
	  cc -I../simavr/sim -o test_avrxt_engine test_avrxt_engine.c \
	     -L../simavr/obj-* -lsimavr -lm && ./test_avrxt_engine
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sim_avr.h"
#include "sim_core.h"
#include "sim_interrupts.h"
#include "sim_regbit.h"
#include "sim_io.h"
#include "sim_irq.h"
#include "avr_twi.h"
#include "avr_twi_modern.h"
#include "avr_port_modern.h"
#include "avr_tcb.h"
#include "avr_tca.h"
#include "avr_usart_modern.h"
#include "avr_nvmctrl.h"
#include "avr_rtc.h"
#include "avr_adc_modern.h"
#include "avr_adc_tiny2.h"
#include "avr_spi_modern.h"
#include "avr_ac.h"
#include "avr_dac.h"
#include "avr_ccl.h"
#include "avr_evsys.h"
#include "avr_portmux.h"
#include "avr_vref.h"
#include "avr_tcd.h"
#include "avr_wdt.h"
#include "avr_watchdog.h"	/* AVR_IOCTL_WATCHDOG_RESET */
#include "avr_crcscan.h"
#include "avr_slpctrl.h"
#include "avr_rstctrl.h"
#include "avr_bod.h"
#include "avr_syscfg.h"

static int failures;

/* Records the latest value seen on an IRQ (for observing PORT pin outputs). */
static uint8_t g_irqval;
static void rec_irq(struct avr_irq_t *irq, uint32_t value, void *param)
{
	(void)irq;
	*(uint8_t *)param = value & 0xff;
}

/* SPI test hooks: an echo "slave" that answers each clocked-out byte with its
 * complement (host test), and a plain capture of bytes seen on an OUTPUT IRQ. */
static avr_t *g_spi_avr;
static uint8_t g_spi_mosi;	/* last byte the host clocked out */
static uint8_t g_spi_out;	/* last byte a client echoed back */
static void spi_echo_hook(struct avr_irq_t *irq, uint32_t value, void *param)
{
	(void)irq; (void)param;
	g_spi_mosi = value & 0xff;
	avr_irq_t *in = avr_io_getirq(g_spi_avr, AVR_IOCTL_SPI_GETIRQ('0'),
								  SPI_IRQ_INPUT);
	avr_raise_irq(in, (value ^ 0xff) & 0xff);	/* reply = complement */
}
static void spi_capture_hook(struct avr_irq_t *irq, uint32_t value, void *param)
{
	(void)irq; (void)param;
	g_spi_out = value & 0xff;
}

/* Captures the last ACK/NACK a modern-TWI slave emits on the wire (the data
 * bit is 1=ACK, 0=NACK). */
static int g_twi_ack_seen;
static uint8_t g_twi_ack_bit;
static void twi_ack_capture_hook(struct avr_irq_t *irq, uint32_t value, void *param)
{
	(void)irq; (void)param;
	avr_twi_msg_irq_t m;
	m.u.v = value;
	if (m.u.twi.msg & TWI_COND_ACK) {
		g_twi_ack_seen = 1;
		g_twi_ack_bit = m.u.twi.data & 1;
	}
}

/* Captures the DAC output (millivolts, full width). */
static uint32_t g_dac_out;
static void dac_capture_hook(struct avr_irq_t *irq, uint32_t value, void *param)
{
	(void)irq; (void)param;
	g_dac_out = value;
}

static uint32_t g_dac_pin;
static void dac_pin_capture_hook(struct avr_irq_t *irq, uint32_t value, void *param)
{
	(void)irq; (void)param;
	g_dac_pin = value;
}

/* Counts the rising edges (value==1) seen on an EVSYS user output. */
static int g_evsys_pulses;
static void evsys_count_hook(struct avr_irq_t *irq, uint32_t value, void *param)
{
	(void)irq; (void)param;
	if (value)
		g_evsys_pulses++;
}

static void check(const char *what, long got, long want)
{
	if (got != want) {
		printf("  FAIL: %-40s got %ld, want %ld\n", what, got, want);
		failures++;
	} else {
		printf("  ok:   %-40s = %ld\n", what, got);
	}
}

/*
 * A tiny register-pointer I2C slave (eeprom-like) used to exercise the modern
 * TWI master over the wire IRQ protocol. The first byte written after a START
 * is the register pointer; subsequent writes store at, and reads fetch from,
 * the auto-incrementing pointer.
 */
typedef struct test_slave_t {
	avr_irq_t	*irq;		// 2 IRQs: [TWI_IRQ_INPUT]=reply, [TWI_IRQ_OUTPUT]=recv
	uint8_t		addr;		// 7-bit slave address
	uint8_t		mem[16];
	uint8_t		ptr;
	uint8_t		selected;
	uint8_t		writephase;	// 0: next write is the register pointer; 1: data
} test_slave_t;

static void test_slave_hook(struct avr_irq_t *irq, uint32_t value, void *param)
{
	(void)irq;
	test_slave_t *s = (test_slave_t *)param;
	avr_twi_msg_irq_t m;
	m.u.v = value;

	if (m.u.twi.msg & TWI_COND_STOP) {
		s->selected = 0;
		s->writephase = 0;
	}
	if (m.u.twi.msg & TWI_COND_START) {
		s->selected = 0;
		s->writephase = 0;
		if ((m.u.twi.addr >> 1) == s->addr) {
			s->selected = m.u.twi.addr;	// includes R/W bit
			avr_raise_irq(s->irq + TWI_IRQ_INPUT,
						  avr_twi_irq_msg(TWI_COND_ACK, s->selected, 1));
		}
	}
	if (s->selected) {
		if (m.u.twi.msg & TWI_COND_WRITE) {
			if (!s->writephase) {
				s->ptr = m.u.twi.data & 0x0f;
				s->writephase = 1;
			} else {
				s->mem[s->ptr++ & 0x0f] = m.u.twi.data;
			}
			avr_raise_irq(s->irq + TWI_IRQ_INPUT,
						  avr_twi_irq_msg(TWI_COND_ACK, s->selected, 1));
		}
		if (m.u.twi.msg & TWI_COND_READ) {
			uint8_t d = s->mem[s->ptr++ & 0x0f];
			avr_raise_irq(s->irq + TWI_IRQ_INPUT,
						  avr_twi_irq_msg(TWI_COND_READ, s->selected, d));
		}
	}
}

/*
 * Simulate a CPU store/load to an I/O register: dispatch through the registered
 * io callback table the way _avr_set_ram()/_avr_get_ram() do (avr_core_watch_*
 * deliberately bypasses the module callbacks).
 */
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

// Put a 16-bit opcode at word address pc/2.
static void put16(avr_t *avr, uint32_t byteaddr, uint16_t op)
{
	avr->flash[byteaddr] = op & 0xff;
	avr->flash[byteaddr + 1] = op >> 8;
}

// Step one instruction; return cycles consumed.
static avr_cycle_count_t step(avr_t *avr)
{
	avr_cycle_count_t before = avr->cycle;
	avr->pc = avr_run_one(avr);
	return avr->cycle - before;
}

// Run one instruction at a fixed pc=0 and report its cycle count, for both
// classic and AVRxt timing, given a 16-bit opcode (and optional 2nd word).
static avr_cycle_count_t cycles_for(avr_t *avr, uint16_t op, int has_word2,
									uint16_t word2, int xt)
{
	if (xt)
		avr->arch.flags |= AVR_ARCH_F_XT_TIMING;
	else
		avr->arch.flags &= ~AVR_ARCH_F_XT_TIMING;
	avr->pc = 0;
	avr->cycle = 0;
	put16(avr, 0, op);
	if (has_word2)
		put16(avr, 2, word2);
	avr->pc = 0;
	avr_cycle_count_t before = avr->cycle;
	avr->pc = avr_run_one(avr);
	return avr->cycle - before;
}


/* Counts rising edges seen on an EVSYS user output into *(int *)param. */
static void evsys_count_param_hook(struct avr_irq_t *irq, uint32_t value, void *param)
{
	(void)irq;
	if (value)
		(*(int *)param)++;
}

/*
 * tinyAVR 2-series helpers. Run NOPs (1 cycle each) until (data[addr] & mask)
 * is set; return the cycles elapsed, or -1 on timeout.
 */
static long run_until_set(avr_t *m, avr_io_addr_t addr, uint8_t mask, long max)
{
	long t0 = (long)m->cycle;
	for (long i = 0; i < max; i++) {
		avr_run(m);
		if (m->pc >= 0x1000)	/* keep the PC inside the NOP-filled flash */
			m->pc = 0;
		if (m->data[addr] & mask)
			return (long)m->cycle - t0;
	}
	return -1;
}

/* Dispatch whatever level-0 interrupt is pending and return its vector number
 * (-1 if none was taken); leaves the CPU out of the ISR again. SEI re-arms the
 * dispatcher (the modern service routine disarms itself while I=0). */
static int take_vector(avr_t *m)
{
	m->sreg[S_I] = 1;
	m->interrupt_state = avr_has_pending_interrupts(m);
	m->pc = 0x100;
	avr_service_interrupts(m);
	int v = -1;
	if (m->pc != 0x100) {
		v = m->pc / m->vector_size;
		avr_interrupt_reti(m);
	}
	m->sreg[S_I] = 0;	/* later avr_run() calls must not take interrupts */
	return v;
}

int main(void)
{
	avr_t *avr = avr_make_mcu_by_name("attiny85");
	if (!avr) { printf("cannot make core\n"); return 2; }
	avr->log = LOG_ERROR;
	avr_init(avr);
	// Give ourselves a sane stack and known register state.
	avr->data[avr->arch.sp_addr] = 0xff;
	avr->data[avr->arch.sp_addr + 1] = 0x02;

	printf("== AVRxt instruction timing ==\n");
	// PUSH r0 (0x920f): AVRe 2, AVRxt 1.
	check("PUSH classic", cycles_for(avr, 0x920f, 0, 0, 0), 2);
	check("PUSH avrxt",   cycles_for(avr, 0x920f, 0, 0, 1), 1);
	// ST Z, r0 (0x8200): AVRe 2, AVRxt 1.
	check("ST Z classic", cycles_for(avr, 0x8200, 0, 0, 0), 2);
	check("ST Z avrxt",   cycles_for(avr, 0x8200, 0, 0, 1), 1);
	// STD Z+1, r0 (0x8201): AVRe 2, AVRxt 1.
	check("STD Z+1 classic", cycles_for(avr, 0x8201, 0, 0, 0), 2);
	check("STD Z+1 avrxt",   cycles_for(avr, 0x8201, 0, 0, 1), 1);
	// LDD r0, Z+1 (0x8001): load stays 2 on both.
	check("LDD Z+1 classic", cycles_for(avr, 0x8001, 0, 0, 0), 2);
	check("LDD Z+1 avrxt",   cycles_for(avr, 0x8001, 0, 0, 1), 2);
	// LDS r0, 0x0100 (0x9000 + word): AVRe 2, AVRxt 3.
	check("LDS classic", cycles_for(avr, 0x9000, 1, 0x0100, 0), 2);
	check("LDS avrxt",   cycles_for(avr, 0x9000, 1, 0x0100, 1), 3);
	// SBI 0x05,0 (0x9a28): AVRe 2, AVRxt 1.
	check("SBI classic", cycles_for(avr, 0x9a28, 0, 0, 0), 2);
	check("SBI avrxt",   cycles_for(avr, 0x9a28, 0, 0, 1), 1);
	// CBI 0x05,0 (0x9828): AVRe 2, AVRxt 1.
	check("CBI classic", cycles_for(avr, 0x9828, 0, 0, 0), 2);
	check("CBI avrxt",   cycles_for(avr, 0x9828, 0, 0, 1), 1);
	// RCALL .0 (0xd000): AVRe 3, AVRxt 2.
	check("RCALL classic", cycles_for(avr, 0xd000, 0, 0, 0), 3);
	check("RCALL avrxt",   cycles_for(avr, 0xd000, 0, 0, 1), 2);
	// CALL 0x000100 (0x940e + word): AVRe 4, AVRxt 3.
	check("CALL classic", cycles_for(avr, 0x940e, 1, 0x0080, 0), 4);
	check("CALL avrxt",   cycles_for(avr, 0x940e, 1, 0x0080, 1), 3);
	// ICALL (0x9509): AVRe 3, AVRxt 2.
	check("ICALL classic", cycles_for(avr, 0x9509, 0, 0, 0), 3);
	check("ICALL avrxt",   cycles_for(avr, 0x9509, 0, 0, 1), 2);
	// Unaffected control case: POP (0x900f) stays 2 on both.
	check("POP classic", cycles_for(avr, 0x900f, 0, 0, 0), 2);
	check("POP avrxt",   cycles_for(avr, 0x900f, 0, 0, 1), 2);

	printf("== CCP window ==\n");
	avr->arch.ccp_window = 0;
	check("ccp initially closed", avr_ccp_io_write_enabled(avr), 0);
	avr_ccp_write(avr, AVR_CCP_IOREG);
	check("ccp open after IOREG sig", avr_ccp_io_write_enabled(avr) > 0, 1);
	avr_ccp_write(avr, 0x12 /* wrong */);
	// wrong signature is a no-op; the previously-armed window is unaffected
	check("ccp still open (window>0)", avr_ccp_io_write_enabled(avr) > 0, 1);

	/*
	 * Datasheet (DS40002205A 8.5.7.1): the protected write must occur "within
	 * four instructions" *following* the CCP write. Model the exact decrement
	 * stream as avr_run_one() applies it (decrement at end of each instruction,
	 * the CCP-writing instruction being k=0), and count how many subsequent
	 * instructions see the window open. Must be exactly AVR_CCP_WINDOW.
	 */
	avr->arch.ccp_window = 0;
	avr_ccp_write(avr, AVR_CCP_IOREG);                 // k=0 body: arm
	if (avr->arch.ccp_window) avr->arch.ccp_window--;  // k=0 end-of-instr decrement
	avr->pc = 0; put16(avr, 0, 0x0000);                // a NOP to step on
	int open = 0;
	for (int k = 1; k <= AVR_CCP_WINDOW + 2; k++) {
		int en = avr_ccp_io_write_enabled(avr);        // checked during instr body
		avr->pc = 0; step(avr);                        // engine decrements at end
		if (en) open++; else break;
	}
	check("window open for exactly AVR_CCP_WINDOW instrs after write",
			open, AVR_CCP_WINDOW);

	printf("== flash mapped into data space ==\n");
	avr->arch.flashmap_start = 0x8000;
	avr->flash[0x0010] = 0xA5;
	avr->flash[0x0011] = 0x5A;
	// LDS r0, 0x8010  -> should read flash[0x10] = 0xA5
	avr->pc = 0; avr->cycle = 0;
	put16(avr, 0, 0x9000); put16(avr, 2, 0x8010);
	avr->data[0] = 0;
	avr->pc = 0;
	avr->pc = avr_run_one(avr);
	check("LDS from mapped flash", avr->data[0], 0xA5);

	printf("== CPUINT interrupt controller ==\n");
	{
		const uint16_t REN = 0x40, RRAISE = 0x41;   // fake enable/flag registers
		// vector 1 = NMI, vectors 5/10/20 = normal (5 used as level-1 candidate)
		static avr_int_vector_t vnmi, v5, v10, v20;
		vnmi.vector = 1; vnmi.nmi = 1; vnmi.raise_sticky = 1;
		vnmi.enable = (avr_regbit_t)AVR_IO_REGBIT(REN, 3);
		vnmi.raised = (avr_regbit_t)AVR_IO_REGBIT(RRAISE, 3);
		v5.vector = 5; v5.raise_sticky = 1;
		v5.enable = (avr_regbit_t)AVR_IO_REGBIT(REN, 0);
		v5.raised = (avr_regbit_t)AVR_IO_REGBIT(RRAISE, 0);
		v10.vector = 10; v10.raise_sticky = 1;
		v10.enable = (avr_regbit_t)AVR_IO_REGBIT(REN, 1);
		v10.raised = (avr_regbit_t)AVR_IO_REGBIT(RRAISE, 1);
		v20.vector = 20; v20.raise_sticky = 1;
		v20.enable = (avr_regbit_t)AVR_IO_REGBIT(REN, 2);
		v20.raised = (avr_regbit_t)AVR_IO_REGBIT(RRAISE, 2);
		avr_register_vector(avr, &vnmi);
		avr_register_vector(avr, &v5);
		avr_register_vector(avr, &v10);
		avr_register_vector(avr, &v20);
		avr->arch.flags |= AVR_ARCH_F_CPUINT | AVR_ARCH_F_MODERN;
		int vs = avr->vector_size;

		#define INT_RESET() do { \
			avr_interrupt_reset(avr); \
			avr->interrupts.cpuint_status = 0; \
			avr->interrupts.cpuint_lvl1vec = 0; \
			avr->interrupts.cpuint_lvl0pri = 0; \
			avr->interrupts.cpuint_lvl0rr = 0; \
			avr->interrupts.running_ptr = 0; \
			avr->arch.ccp_window = 0; \
			avr->data[REN] = 0xff; avr->data[RRAISE] = 0; \
		} while (0)

		// A. Level-0 dispatch: jumps to vector, sets LVL0EX, leaves I set.
		INT_RESET();
		avr->sreg[S_I] = 1;
		avr->pc = 0x100;
		avr_raise_interrupt(avr, &v10);
		avr_service_interrupts(avr);
		check("LVL0 dispatch -> vector*vsize", avr->pc, 10 * vs);
		check("LVL0 sets LVL0EX", avr_cpuint_get_status(avr) & AVR_CPUINT_LVL0EX ? 1 : 0, 1);
		check("entry does NOT clear I", avr->sreg[S_I], 1);
		check("flag stays raised (sticky)", avr_regbit_get(avr, v10.raised), 1);
		// RETI clears the level flag, leaves I alone.
		avr_interrupt_reti(avr);
		check("RETI clears LVL0EX", avr_cpuint_get_status(avr) & AVR_CPUINT_LVL0EX ? 1 : 0, 0);
		check("RETI does NOT touch I", avr->sreg[S_I], 1);

		// B. NMI is serviced even with global interrupts disabled (I=0).
		INT_RESET();
		avr->sreg[S_I] = 0;
		avr->pc = 0x100;
		avr_raise_interrupt(avr, &vnmi);
		avr_service_interrupts(avr);
		check("NMI dispatched with I=0", avr->pc, 1 * vs);
		check("NMI sets NMIEX", avr_cpuint_get_status(avr) & AVR_CPUINT_NMIEX ? 1 : 0, 1);

		// C. Level-1 preempts a running level-0 handler.
		INT_RESET();
		avr->sreg[S_I] = 1;
		avr->interrupts.cpuint_lvl1vec = 5;             // v5 is now level 1
		avr->interrupts.cpuint_status = AVR_CPUINT_LVL0EX;  // pretend in a LVL0 ISR
		avr->pc = 0x100;
		avr_raise_interrupt(avr, &v5);
		avr_service_interrupts(avr);
		check("LVL1 preempts LVL0", avr->pc, 5 * vs);
		check("LVL1 sets LVL1EX", avr_cpuint_get_status(avr) & AVR_CPUINT_LVL1EX ? 1 : 0, 1);

		// D. Level-0 does NOT preempt a running level-0 handler.
		INT_RESET();
		avr->sreg[S_I] = 1;
		avr->interrupts.cpuint_status = AVR_CPUINT_LVL0EX;  // in a LVL0 ISR
		avr->pc = 0x100;
		avr_raise_interrupt(avr, &v10);
		avr_service_interrupts(avr);
		check("LVL0 does not preempt LVL0 (pc unchanged)", avr->pc, 0x100);

		// E. Static priority: lowest vector number wins by default.
		INT_RESET();
		avr->sreg[S_I] = 1;
		avr->pc = 0x100;
		avr_raise_interrupt(avr, &v20);
		avr_raise_interrupt(avr, &v10);
		avr_service_interrupts(avr);
		check("static priority: lower vector first", avr->pc, 10 * vs);

		// F. Modified static via LVL0PRI: vector LVL0PRI+1 gets highest priority.
		INT_RESET();
		avr->sreg[S_I] = 1;
		avr->interrupts.cpuint_lvl0pri = 19;   // -> vector 20 highest priority
		avr->pc = 0x100;
		avr_raise_interrupt(avr, &v20);
		avr_raise_interrupt(avr, &v10);
		avr_service_interrupts(avr);
		check("LVL0PRI=19 makes vector 20 win", avr->pc, 20 * vs);

		// G. CCP window suppresses interrupt servicing.
		INT_RESET();
		avr->sreg[S_I] = 1;
		avr->arch.ccp_window = 2;
		avr->pc = 0x100;
		avr_raise_interrupt(avr, &v10);
		avr_service_interrupts(avr);
		check("CCP window suppresses interrupts", avr->pc, 0x100);
		avr->arch.ccp_window = 0;
		avr_service_interrupts(avr);
		check("serviced once CCP window closes", avr->pc, 10 * vs);

		// H. Compact Vector Table: all LVL0 dispatch to vector 3, the level-1
		// vector to 2, NMI to 1 (datasheet 13.3.2.5). Selection is unchanged.
		avr->interrupts.cpuint_cvt = 1;
		INT_RESET();
		avr->sreg[S_I] = 1;
		avr->pc = 0x100;
		avr_raise_interrupt(avr, &v10);
		avr_service_interrupts(avr);
		check("CVT: LVL0 dispatches to vector 3", avr->pc, 3 * vs);

		INT_RESET();
		avr->sreg[S_I] = 1;
		avr->interrupts.cpuint_lvl1vec = 5;	// v5 is level 1
		avr->pc = 0x100;
		avr_raise_interrupt(avr, &v5);
		avr_service_interrupts(avr);
		check("CVT: LVL1 dispatches to vector 2", avr->pc, 2 * vs);

		INT_RESET();
		avr->sreg[S_I] = 0;			// NMI ignores I
		avr->pc = 0x100;
		avr_raise_interrupt(avr, &vnmi);
		avr_service_interrupts(avr);
		check("CVT: NMI dispatches to vector 1", avr->pc, 1 * vs);
		avr->interrupts.cpuint_cvt = 0;

		// I. IVSEL relocates the vector base (DS40002205A 13.5.1). With
		// FUSE.BOOTEND=4 the boot section is 4*256=0x400 bytes; the application
		// section (IVSEL=0) starts there, the boot section (IVSEL=1) at 0.
		avr->interrupts.cpuint_bootend_idx = 8;
		avr->fuse[8] = 4;				// BOOTEND*256 = 0x400 app-section base
		INT_RESET();
		avr->sreg[S_I] = 1;
		avr->interrupts.cpuint_ivsel = 0;		// vectors at application section
		avr->pc = 0x100;
		avr_raise_interrupt(avr, &v10);
		avr_service_interrupts(avr);
		check("IVSEL=0 dispatches at BOOTEND*256 + vector*vsize",
				avr->pc, 0x400 + 10 * vs);

		INT_RESET();
		avr->sreg[S_I] = 1;
		avr->interrupts.cpuint_ivsel = 1;		// vectors at boot section (0)
		avr->pc = 0x100;
		avr_raise_interrupt(avr, &v10);
		avr_service_interrupts(avr);
		check("IVSEL=1 dispatches at boot section (base 0)", avr->pc, 10 * vs);

		// With BOOTEND=0 the whole flash is boot, so IVSEL=0 has no effect.
		avr->fuse[8] = 0;
		INT_RESET();
		avr->sreg[S_I] = 1;
		avr->interrupts.cpuint_ivsel = 0;
		avr->pc = 0x100;
		avr_raise_interrupt(avr, &v10);
		avr_service_interrupts(avr);
		check("BOOTEND=0: IVSEL=0 base stays 0", avr->pc, 10 * vs);
		avr->interrupts.cpuint_bootend_idx = 0xff;
		avr->interrupts.cpuint_ivsel = 0;

		// Clearing a queued-but-unserviced vector must dequeue it. Stale
		// entries used to pile up until the 64-entry FIFO was full, after
		// which a newly raised vector was never queued nor dispatched.
		INT_RESET();
		avr->sreg[S_I] = 0;
		for (int k = 0; k < 70; k++) {
			avr_raise_interrupt(avr, &v10);
			avr_clear_interrupt(avr, &v10);
		}
		check("raise/clear x70 leaves nothing pending",
				avr_has_pending_interrupts(avr), 0);
		avr_raise_interrupt(avr, &v20);
		avr->sreg[S_I] = 1;
		avr->pc = 0x100;
		avr_service_interrupts(avr);
		check("vector raised after x70 raise/clear dispatches",
				avr->pc, 20 * vs);

		#undef INT_RESET
	}

	printf("== modern TWI0 (I2C) ==\n");
	{
		/* Register-bit shorthands (see avr_twi_modern.c) */
		enum { MENABLE = 0x01 };
		enum { BUS_IDLE = 0x01, ST_RXACK = 0x10, ST_WIF = 0x40, ST_RIF = 0x80 };
		enum { CMD_RECVTRANS = 0x02, CMD_STOP = 0x03 };
		enum { SENABLE = 0x01, SAPIEN = 0x40, SDIEN = 0x80 };
		enum { SCMD_RESPONSE = 0x03 };
		enum { SS_AP = 0x01, SS_APIF = 0x40, SS_DIF = 0x80 };

		/*
		 * A modern peripheral lives at data 0x810, well above any classic
		 * core's IO window. Build a core with an enlarged IO/data span and the
		 * modern arch flags, then attach the modern TWI there. (This stands in
		 * for the full Phase-5 ATtiny3217 core.)
		 */
		avr_t *tw = avr_make_mcu_by_name("atmega2560");
		if (!tw) { printf("cannot make core for TWI\n"); return 2; }
		tw->log = LOG_ERROR;
		tw->ramend = 0x3fff;
		tw->ioend = 0x1fff;
		tw->arch.flags |= AVR_ARCH_F_MODERN | AVR_ARCH_F_CPUINT;
		tw->arch.io_offset = 0;
		tw->arch.sp_addr = 0x3d;
		tw->arch.sreg_addr = 0x3f;
		avr_init(tw);

		static avr_twi_modern_t twi;
		const avr_io_addr_t B = 0x810;
		avr_twi_modern_init(tw, &twi, B, 25 /*TWIM*/, 24 /*TWIS*/, '0');

		/* Attach the loopback slave at address 0x20 to the TWI wire IRQs. */
		static const char *snames[2] = { "8>slave.out", "32<slave.in" };
		static test_slave_t slave;
		memset(&slave, 0, sizeof(slave));
		slave.addr = 0x20;
		slave.irq = avr_alloc_irq(&tw->irq_pool, 0, 2, snames);
		avr_irq_register_notify(slave.irq + TWI_IRQ_OUTPUT, test_slave_hook, &slave);
		uint32_t ioctl = AVR_IOCTL_TWI_GETIRQ('0');
		avr_connect_irq(slave.irq + TWI_IRQ_INPUT,
						avr_io_getirq(tw, ioctl, TWI_IRQ_INPUT));
		avr_connect_irq(avr_io_getirq(tw, ioctl, TWI_IRQ_OUTPUT),
						slave.irq + TWI_IRQ_OUTPUT);
		/* Tap the wire to observe the ACK/NACK our own slave role emits. */
		avr_irq_register_notify(avr_io_getirq(tw, ioctl, TWI_IRQ_OUTPUT),
								twi_ack_capture_hook, NULL);

		/* ---- Master: enable, force idle ---- */
		cpu_write(tw, B + TWIM_MCTRLA, MENABLE);
		cpu_write(tw, B + TWIM_MSTATUS, BUS_IDLE);
		check("MSTATUS forced to IDLE",
			  tw->data[B + TWIM_MSTATUS] & 0x03, BUS_IDLE);

		/* ---- Master write transaction to 0x20: ptr=2, data 0xAB,0xCD ---- */
		cpu_write(tw, B + TWIM_MADDR, (0x20 << 1) | 0);
		check("addr ACKed (RXACK=0)", tw->data[B + TWIM_MSTATUS] & ST_RXACK, 0);
		check("WIF set after address (write)",
			  !!(tw->data[B + TWIM_MSTATUS] & ST_WIF), 1);
		cpu_write(tw, B + TWIM_MDATA, 0x02);	/* register pointer */
		cpu_write(tw, B + TWIM_MDATA, 0xAB);
		cpu_write(tw, B + TWIM_MDATA, 0xCD);
		check("WIF set after data byte",
			  !!(tw->data[B + TWIM_MSTATUS] & ST_WIF), 1);
		cpu_write(tw, B + TWIM_MCTRLB, CMD_STOP);
		check("slave mem[2] written", slave.mem[2], 0xAB);
		check("slave mem[3] written", slave.mem[3], 0xCD);
		check("STOP returns bus to IDLE",
			  tw->data[B + TWIM_MSTATUS] & 0x03, BUS_IDLE);

		/* ---- Set the read pointer back to 2 (write ptr only, then stop) ---- */
		cpu_write(tw, B + TWIM_MADDR, (0x20 << 1) | 0);
		cpu_write(tw, B + TWIM_MDATA, 0x02);
		cpu_write(tw, B + TWIM_MCTRLB, CMD_STOP);

		/* ---- Master read transaction from 0x20 ---- */
		cpu_write(tw, B + TWIM_MADDR, (0x20 << 1) | 1);
		check("RIF set after read address", !!(tw->data[B + TWIM_MSTATUS] & ST_RIF), 1);
		check("first read byte == mem[2]", cpu_read(tw, B + TWIM_MDATA), 0xAB);
		check("RIF cleared after MDATA read", !!(tw->data[B + TWIM_MSTATUS] & ST_RIF), 0);
		cpu_write(tw, B + TWIM_MCTRLB, CMD_RECVTRANS);	/* ACK + next */
		check("second read byte == mem[3]", cpu_read(tw, B + TWIM_MDATA), 0xCD);
		cpu_write(tw, B + TWIM_MCTRLB, CMD_STOP);

		/* ---- Master addresses an absent slave: expect NACK ---- */
		cpu_write(tw, B + TWIM_MADDR, (0x30 << 1) | 0);
		check("absent slave -> RXACK=1 (NACK)",
			  !!(tw->data[B + TWIM_MSTATUS] & ST_RXACK), 1);
		cpu_write(tw, B + TWIM_MCTRLB, CMD_STOP);

		/* ---- Slave role: remote master writes a byte to us at 0x33 ---- */
		cpu_write(tw, B + TWIM_SADDR, 0x33 << 1);
		cpu_write(tw, B + TWIM_SCTRLA, SENABLE | SAPIEN | SDIEN);
		avr_irq_t *in = avr_io_getirq(tw, ioctl, TWI_IRQ_INPUT);

		avr_raise_irq(in, avr_twi_irq_msg(TWI_COND_START | TWI_COND_ADDR,
										  (0x33 << 1) | 0, 0));
		check("slave APIF set on address match",
			  !!(tw->data[B + TWIM_SSTATUS] & SS_APIF), 1);
		check("slave AP=1 (address)", !!(tw->data[B + TWIM_SSTATUS] & SS_AP), 1);
		check("slave TWIS interrupt pending",
			  avr_is_interrupt_pending(tw, &twi.svector), 1);
		cpu_write(tw, B + TWIM_SCTRLB, SCMD_RESPONSE);	/* ACK addr */
		check("slave APIF cleared by command",
			  !!(tw->data[B + TWIM_SSTATUS] & SS_APIF), 0);

		avr_raise_irq(in, avr_twi_irq_msg(TWI_COND_WRITE, (0x33 << 1) | 0, 0x77));
		check("slave received data into SDATA", tw->data[B + TWIM_SDATA], 0x77);
		check("slave DIF set on data", !!(tw->data[B + TWIM_SSTATUS] & SS_DIF), 1);
		cpu_write(tw, B + TWIM_SCTRLB, SCMD_RESPONSE);

		avr_raise_irq(in, avr_twi_irq_msg(TWI_COND_STOP, (0x33 << 1) | 0, 0));
		check("slave APIF set on STOP",
			  !!(tw->data[B + TWIM_SSTATUS] & SS_APIF), 1);

		/* ---- Slave honours SCTRLB.ACKACT: NACK the next address ---- */
		enum { S_ACKACT = 0x04 };
		/* Clear the lingering APIF and arm ACKACT=NACK in one write (the
		 * datasheet permits ACKACT and SCMD together, DS40002205A 26.5.10). */
		cpu_write(tw, B + TWIM_SCTRLB, SCMD_RESPONSE | S_ACKACT);
		check("APIF cleared before ACKACT test",
			  !!(tw->data[B + TWIM_SSTATUS] & SS_APIF), 0);
		g_twi_ack_seen = 0;
		avr_raise_irq(in, avr_twi_irq_msg(TWI_COND_START | TWI_COND_ADDR,
										  (0x33 << 1) | 0, 0));
		check("slave NACKs the address when ACKACT=1",
			  g_twi_ack_seen && g_twi_ack_bit == 0, 1);
		check("slave still raises APIF on a NACKed address",
			  !!(tw->data[B + TWIM_SSTATUS] & SS_APIF), 1);
		/* A NACKed address leaves the slave unselected: a following data byte
		 * is dropped rather than latched into SDATA. */
		cpu_write(tw, B + TWIM_SDATA, 0x00);
		avr_raise_irq(in, avr_twi_irq_msg(TWI_COND_WRITE, (0x33 << 1) | 0, 0x99));
		check("NACKed slave ignores the following write byte",
			  tw->data[B + TWIM_SDATA], 0x00);
	}

	printf("== sim_tiny3217 core: TWI0 wired in ==\n");
	{
		enum { MENABLE = 0x01, BUS_IDLE = 0x01, ST_WIF = 0x40, ST_RIF = 0x80 };
		enum { CMD_STOP = 0x03 };
		const avr_io_addr_t B = 0x810;

		avr_t *m = avr_make_mcu_by_name("attiny3217");
		if (!m) { printf("cannot make attiny3217 core\n"); return 2; }
		m->log = LOG_ERROR;
		avr_init(m);

		/* The descriptor must put the engine in modern mode. */
		check("attiny3217 is MODERN", !!(m->arch.flags & AVR_ARCH_F_MODERN), 1);
		check("attiny3217 CPUINT enabled", !!(m->arch.flags & AVR_ARCH_F_CPUINT), 1);
		check("attiny3217 SP at 0x3D", m->arch.sp_addr, 0x3d);
		check("attiny3217 SREG at 0x3F", m->arch.sreg_addr, 0x3f);
		check("attiny3217 CCP at 0x34", m->arch.ccp_addr, 0x34);
		check("attiny3217 flashmap 0x8000", m->arch.flashmap_start, 0x8000);
		check("attiny3217 vector_size 4", m->vector_size, 4);
		check("attiny3217 RAMEND 0x3FFF", m->ramend, 0x3fff);
		check("attiny3217 signature[0]", m->signature[0], 0x1e);
		check("attiny3217 signature[2]", m->signature[2], 0x22);

		/* TWI0 must be registered: MADDR/MCTRLB have write callbacks at 0x810+. */
		check("TWI0 MADDR write hook present",
			  !!m->io[AVR_DATA_TO_IO(B + TWIM_MADDR)].w.c, 1);
		check("TWI0 MCTRLB write hook present",
			  !!m->io[AVR_DATA_TO_IO(B + TWIM_MCTRLB)].w.c, 1);

		/* Drive a real transaction against a loopback slave. */
		static const char *snames[2] = { "8>slave.out", "32<slave.in" };
		static test_slave_t slave;
		memset(&slave, 0, sizeof(slave));
		slave.addr = 0x20;
		slave.irq = avr_alloc_irq(&m->irq_pool, 0, 2, snames);
		avr_irq_register_notify(slave.irq + TWI_IRQ_OUTPUT, test_slave_hook, &slave);
		uint32_t ioctl = AVR_IOCTL_TWI_GETIRQ('0');
		avr_connect_irq(slave.irq + TWI_IRQ_INPUT,
						avr_io_getirq(m, ioctl, TWI_IRQ_INPUT));
		avr_connect_irq(avr_io_getirq(m, ioctl, TWI_IRQ_OUTPUT),
						slave.irq + TWI_IRQ_OUTPUT);

		cpu_write(m, B + TWIM_MCTRLA, MENABLE);
		cpu_write(m, B + TWIM_MSTATUS, BUS_IDLE);
		cpu_write(m, B + TWIM_MADDR, (0x20 << 1) | 0);
		check("core: WIF after address", !!(m->data[B + TWIM_MSTATUS] & ST_WIF), 1);
		cpu_write(m, B + TWIM_MDATA, 0x05);	/* reg pointer */
		cpu_write(m, B + TWIM_MDATA, 0x42);	/* data */
		cpu_write(m, B + TWIM_MCTRLB, CMD_STOP);
		check("core: slave mem[5] written via TWI0", slave.mem[5], 0x42);
	}

	printf("== modern CPUINT register block (sim_tiny3217) ==\n");
	{
		/* CPUINT register map (datasheet DS40002205A §13.4): the controller
		 * lives at 0x0110 on every modern part; the engine reads its priority
		 * config from these registers via the avr_cpuint bridge. */
		enum { CTRLA = 0x110, STATUS = 0x111, LVL0PRI = 0x112, LVL1VEC = 0x113 };
		enum { LVL0RR_bm = 0x01, CVT_bm = 0x20, IVSEL_bm = 0x40 };

		avr_t *m = avr_make_mcu_by_name("attiny3217");
		if (!m) { printf("cannot make attiny3217 core\n"); return 2; }
		m->log = LOG_ERROR;
		avr_init(m);

		/* The core descriptor must have wired the CPUINT block in. */
		check("CPUINT CTRLA write hook present",
			  !!m->io[AVR_DATA_TO_IO(CTRLA)].w.c, 1);
		check("CPUINT STATUS read hook present",
			  !!m->io[AVR_DATA_TO_IO(STATUS)].r.c, 1);

		/* LVL1VEC: firmware-selected level-1 vector reaches the engine. */
		cpu_write(m, LVL1VEC, 5);
		check("LVL1VEC write -> engine", m->interrupts.cpuint_lvl1vec, 5);
		check("LVL1VEC reads back", cpu_read(m, LVL1VEC), 5);

		/* LVL0PRI: firmware write reaches the engine; reads return the live
		 * value (hardware updates it under round robin). */
		cpu_write(m, LVL0PRI, 19);
		check("LVL0PRI write -> engine", m->interrupts.cpuint_lvl0pri, 19);
		m->interrupts.cpuint_lvl0pri = 7;	/* simulate a round-robin ack */
		check("LVL0PRI read is live", cpu_read(m, LVL0PRI), 7);

		/* CTRLA.LVL0RR is NOT CCP-protected: it takes effect immediately. */
		cpu_write(m, CTRLA, LVL0RR_bm);
		check("LVL0RR write -> engine (no CCP)", m->interrupts.cpuint_lvl0rr, 1);

		/* CTRLA.CVT and IVSEL ARE CCP-protected: a write without an unlock is
		 * dropped for those bits (datasheet Table 13-3). */
		cpu_write(m, CTRLA, LVL0RR_bm | CVT_bm | IVSEL_bm);
		check("CVT rejected without CCP", m->interrupts.cpuint_cvt, 0);
		check("CVT bit not stored without CCP", !!(m->data[CTRLA] & CVT_bm), 0);
		check("IVSEL bit not stored without CCP", !!(m->data[CTRLA] & IVSEL_bm), 0);

		/* Unlock CCP (write the IOREG signature to CPU.CCP), then the protected
		 * bits take effect. No instructions run between the writes here, so the
		 * window stays open. */
		cpu_write(m, m->arch.ccp_addr, 0xD8 /* CCP_IOREG */);
		cpu_write(m, CTRLA, LVL0RR_bm | CVT_bm);
		check("CVT accepted with CCP open", m->interrupts.cpuint_cvt, 1);
		check("CVT bit stored with CCP open", !!(m->data[CTRLA] & CVT_bm), 1);

		/* STATUS is read-only to firmware: it mirrors the live engine flags and
		 * ignores writes. */
		m->interrupts.cpuint_status = AVR_CPUINT_LVL1EX | AVR_CPUINT_LVL0EX;
		check("STATUS read mirrors engine", cpu_read(m, STATUS),
			  AVR_CPUINT_LVL1EX | AVR_CPUINT_LVL0EX);
		cpu_write(m, STATUS, 0x00);
		check("STATUS write ignored (read-only)", m->interrupts.cpuint_status,
			  AVR_CPUINT_LVL1EX | AVR_CPUINT_LVL0EX);
	}

	printf("== modern PORT/VPORT (sim_tiny3217) ==\n");
	{
		avr_t *m = avr_make_mcu_by_name("attiny3217");
		if (!m) { printf("cannot make attiny3217 core\n"); return 2; }
		m->log = LOG_ERROR;
		avr_init(m);

		const avr_io_addr_t PA = 0x400;
		uint32_t ioctlA = AVR_IOCTL_IOPORT_GETIRQ('A');

		/* PORTA register hooks present (DIR/OUT/IN/aliases). */
		check("PORTA DIR write hook", !!m->io[AVR_DATA_TO_IO(PA+PORTM_DIR)].w.c, 1);
		check("PORTA OUTSET write hook", !!m->io[AVR_DATA_TO_IO(PA+PORTM_OUTSET)].w.c, 1);

		/* Observe the aggregate output pin value. */
		g_irqval = 0;
		avr_irq_register_notify(avr_io_getirq(m, ioctlA, IOPORT_IRQ_PIN_ALL),
								rec_irq, &g_irqval);

		/* All outputs, drive 0x55. */
		cpu_write(m, PA + PORTM_DIR, 0xff);
		cpu_write(m, PA + PORTM_OUT, 0x55);
		check("PORTA.OUT = 0x55", m->data[PA + PORTM_OUT], 0x55);
		check("PORTA pins driven 0x55", g_irqval, 0x55);

		/* SET / CLR / TGL strobes modify OUT and read back OUT. */
		cpu_write(m, PA + PORTM_OUTSET, 0x80);
		check("OUTSET -> OUT 0xD5", m->data[PA + PORTM_OUT], 0xd5);
		cpu_write(m, PA + PORTM_OUTCLR, 0x05);
		check("OUTCLR -> OUT 0xD0", m->data[PA + PORTM_OUT], 0xd0);
		cpu_write(m, PA + PORTM_OUTTGL, 0xff);
		check("OUTTGL -> OUT 0x2F", m->data[PA + PORTM_OUT], 0x2f);
		check("OUTSET reads back OUT (alias)", cpu_read(m, PA + PORTM_OUTSET), 0x2f);
		check("IN reflects OUT for output pins", cpu_read(m, PA + PORTM_IN), 0x2f);

		/* Writing IN toggles OUT (modern behaviour). */
		cpu_write(m, PA + PORTM_IN, 0x0f);
		check("write IN toggles OUT -> 0x20", m->data[PA + PORTM_OUT], 0x20);

		/* DIRSET/CLR aliases. */
		cpu_write(m, PA + PORTM_DIR, 0x00);
		cpu_write(m, PA + PORTM_DIRSET, 0xf0);
		check("DIRSET -> DIR 0xF0", m->data[PA + PORTM_DIR], 0xf0);
		cpu_write(m, PA + PORTM_DIRCLR, 0x10);
		check("DIRCLR -> DIR 0xE0", m->data[PA + PORTM_DIR], 0xe0);

		/* ---- VPORT alias via *executed* SBI/CBI/STS (exercises the engine
		 * low-I/O redirect; VPORTA is at data 0x00). ---- */
		cpu_write(m, PA + PORTM_DIR, 0xff);	/* all output so OUT drives */
		cpu_write(m, PA + PORTM_OUT, 0x00);
		/* SBI VPORTA.OUT(0x01), bit0  ->  0x9A00 | (1<<3) | 0 */
		m->pc = 0; put16(m, 0, 0x9a08); m->pc = avr_run_one(m);
		check("SBI VPORTA.OUT,0 -> PORTA.OUT bit0", m->data[PA + PORTM_OUT] & 1, 1);
		/* CBI VPORTA.OUT(0x01), bit0  ->  0x9800 | (1<<3) | 0 */
		m->pc = 0; put16(m, 0, 0x9808); m->pc = avr_run_one(m);
		check("CBI VPORTA.OUT,0 -> PORTA.OUT bit0 clr", m->data[PA + PORTM_OUT] & 1, 0);
		/* STS VPORTA.OUT(0x0001), r16 with r16=0xAA  -> 0x9300 + addr word */
		m->data[16] = 0xaa;
		m->pc = 0; put16(m, 0, 0x9300); put16(m, 2, 0x0001); m->pc = avr_run_one(m);
		check("STS VPORTA.OUT,r16 -> PORTA.OUT 0xAA", m->data[PA + PORTM_OUT], 0xaa);
		/* The redirect must NOT have touched GP register r1 (VPORTA.OUT==addr1). */
		check("GP register r1 untouched by VPORT", m->data[1], 0);

		/* ---- GPIOR0-3 (data 0x1C-0x1F) must not alias r28-r31 ---- */
		m->data[28] = 0x11; m->data[31] = 0x22;
		m->data[16] = 0x5a;
		m->pc = 0; put16(m, 0, 0x9300); put16(m, 2, 0x001c); m->pc = avr_run_one(m);	/* STS GPIOR0,r16 */
		check("STS GPIOR0 leaves r28 alone", m->data[28], 0x11);
		m->data[16] = 0xc3;
		m->pc = 0; put16(m, 0, 0x9300); put16(m, 2, 0x001f); m->pc = avr_run_one(m);	/* STS GPIOR3,r16 */
		check("STS GPIOR3 leaves r31 alone", m->data[31], 0x22);
		m->data[17] = 0;
		m->pc = 0; put16(m, 0, 0x9110); put16(m, 2, 0x001c); m->pc = avr_run_one(m);	/* LDS r17,GPIOR0 */
		check("LDS GPIOR0 reads back 0x5A, not r28", m->data[17], 0x5a);
		m->pc = 0; put16(m, 0, 0x9110); put16(m, 2, 0x001f); m->pc = avr_run_one(m);	/* LDS r17,GPIOR3 */
		check("LDS GPIOR3 reads back 0xC3, not r31", m->data[17], 0xc3);
		m->data[28] = 0x77;	/* a register write must not change GPIOR0 */
		m->pc = 0; put16(m, 0, 0x9110); put16(m, 2, 0x001c); m->pc = avr_run_one(m);
		check("r28 write does not change GPIOR0", m->data[17], 0x5a);

		/* ---- Pin-change interrupt via PINnCTRL.ISC ---- */
		enum { ISC_RISING_VAL = 2 };
		cpu_write(m, PA + PORTM_DIR, 0x00);			/* all inputs */
		cpu_write(m, PA + PORTM_PIN0CTRL + 2, ISC_RISING_VAL);	/* pin2 rising */
		avr_raise_irq(avr_io_getirq(m, ioctlA, IOPORT_IRQ_PIN2), 1);
		check("pin2 rising sets INTFLAGS bit2",
			  !!(m->data[PA + PORTM_INTFLAGS] & 0x04), 1);
		check("PORTA_PORT interrupt pending", avr_has_pending_interrupts(m), 1);
		cpu_write(m, PA + PORTM_INTFLAGS, 0x04);	/* W1C */
		check("INTFLAGS cleared by W1C", m->data[PA + PORTM_INTFLAGS], 0);
	}

	printf("== modern PORT pull-up / INVEN (sim_tiny3217) ==\n");
	{
		/* PINnCTRL.PULLUPEN (bit 3) enables the internal pull-up on an input-only
		 * pin; INVEN (bit 7) inverts the resolved level (DS40002205A 16.5.11). An
		 * external driver overrides the weak internal pull-up. */
		enum { PULLUPEN = 0x08, INVEN = 0x80 };

		avr_t *m = avr_make_mcu_by_name("attiny3217");
		if (!m) { printf("cannot make attiny3217 core\n"); return 2; }
		m->log = LOG_ERROR;
		avr_init(m);

		const avr_io_addr_t PA = 0x400;

		/* All inputs, undriven. Without a pull-up an undriven input reads 0. */
		cpu_write(m, PA + PORTM_DIR, 0x00);
		check("undriven input pin0 reads 0", cpu_read(m, PA + PORTM_IN) & 1, 0);

		/* Enable pull-up on pin0 -> IN reads high. */
		cpu_write(m, PA + PORTM_PIN0CTRL + 0, PULLUPEN);
		check("input pin0 with pull-up reads high", cpu_read(m, PA + PORTM_IN) & 1, 1);

		/* An external driver pulling pin0 low overrides the internal pull-up. */
		avr_ioport_external_t ext = { .name = 'A', .mask = 0x01, .value = 0x00 };
		avr_ioctl(m, AVR_IOCTL_IOPORT_SET_EXTERNAL('A'), &ext);
		check("external low overrides internal pull-up", cpu_read(m, PA + PORTM_IN) & 1, 0);

		/* Release the external drive; the pull-up reasserts high on the next
		 * re-evaluation (a PINnCTRL re-write). */
		ext.mask = 0x00;
		avr_ioctl(m, AVR_IOCTL_IOPORT_SET_EXTERNAL('A'), &ext);
		cpu_write(m, PA + PORTM_PIN0CTRL + 0, PULLUPEN);
		check("pull-up reasserts high after external release",
			  cpu_read(m, PA + PORTM_IN) & 1, 1);

		/* INVEN inverts the resolved input level: pulled-up + inverted reads 0. */
		cpu_write(m, PA + PORTM_PIN0CTRL + 0, PULLUPEN | INVEN);
		check("pulled-up input with INVEN reads 0", cpu_read(m, PA + PORTM_IN) & 1, 0);

		/* The global no-pullups option disables the model (AVR_MCU_PORT_NO_PULL). */
		m->options.no_pullups = 1;
		cpu_write(m, PA + PORTM_PIN0CTRL + 1, PULLUPEN);
		check("no_pullups option: pulled input pin1 stays 0",
			  cpu_read(m, PA + PORTM_IN) & 0x02, 0);
		m->options.no_pullups = 0;
	}

	printf("== modern CLKCTRL (sim_tiny3217) ==\n");
	{
		enum { MCLKCTRLA = 0x60, MCLKCTRLB = 0x61, MCLKLOCK = 0x62 };
		enum { PEN = 0x01 };
		/* PDIV field values are shifted left by 1 in MCLKCTRLB. */
		#define PDIV(_field) (((_field) & 0x0f) << 1)

		avr_t *c = avr_make_mcu_by_name("attiny3217");
		if (!c) { printf("cannot make attiny3217 core\n"); return 2; }
		c->log = LOG_ERROR;
		avr_init(c);

		/* Reset default: OSC20M / 6 = 3.333 MHz. */
		check("CLKCTRL reset freq = 20M/6", c->frequency, 3333333);

		/* Disable the prescaler (CCP-protected write): full 20 MHz. */
		avr_ccp_write(c, AVR_CCP_IOREG);
		cpu_write(c, MCLKCTRLB, 0x00);
		check("PEN=0 -> 20 MHz", c->frequency, 20000000);

		/* Prescaler /4 (PDIV field 0x1 = 4X). */
		avr_ccp_write(c, AVR_CCP_IOREG);
		cpu_write(c, MCLKCTRLB, PEN | PDIV(0x1));
		check("PDIV 4X -> 5 MHz", c->frequency, 5000000);

		/* Prescaler /6 (PDIV field 0x8 = 6X). */
		avr_ccp_write(c, AVR_CCP_IOREG);
		cpu_write(c, MCLKCTRLB, PEN | PDIV(0x8));
		check("PDIV 6X -> 3.333 MHz", c->frequency, 3333333);

		/* Without an open CCP window the protected write is ignored. */
		c->arch.ccp_window = 0;
		cpu_write(c, MCLKCTRLB, 0x00);
		check("write without CCP ignored", c->frequency, 3333333);

		/* Switch source to the 32.768 kHz ULP oscillator, no prescaler. */
		avr_ccp_write(c, AVR_CCP_IOREG);
		cpu_write(c, MCLKCTRLB, 0x00);
		avr_ccp_write(c, AVR_CCP_IOREG);
		cpu_write(c, MCLKCTRLA, 0x01);	/* CLKSEL = OSCULP32K */
		check("CLKSEL OSCULP32K -> 32768 Hz", c->frequency, 32768);

		/* Lock the clock config; further protected writes are ignored. */
		avr_ccp_write(c, AVR_CCP_IOREG);
		cpu_write(c, MCLKLOCK, 0x01);	/* LOCKEN */
		avr_ccp_write(c, AVR_CCP_IOREG);
		cpu_write(c, MCLKCTRLA, 0x00);	/* try back to OSC20M */
		check("locked: CLKSEL change ignored", c->frequency, 32768);
		#undef PDIV
	}

	printf("== modern TCB (sim_tiny3217) ==\n");
	{
		const avr_io_addr_t TB = 0xa40;	/* TCB0 */
		enum { TCB_ENABLE = 0x01, TCB_CLKDIV2 = (1 << 1) };
		enum { TCB_CAPT = 0x01, TCB_RUN = 0x01 };

		avr_t *m = avr_make_mcu_by_name("attiny3217");
		if (!m) { printf("cannot make attiny3217 core\n"); return 2; }
		m->log = LOG_ERROR;
		avr_init(m);
		memset(m->flash, 0, 0x2000);	/* a field of NOPs to run (1 cycle each) */

		/* Periodic interrupt every (CCMP+1) = 101 CLK_PER cycles. */
		cpu_write(m, TB + TCBR_CCMPL, 100);
		cpu_write(m, TB + TCBR_CCMPH, 0);
		cpu_write(m, TB + TCBR_INTCTRL, 0x01);	/* CAPT enable */
		cpu_write(m, TB + TCBR_CTRLB, 0x00);	/* CNTMODE = INT */
		cpu_write(m, TB + TCBR_CTRLA, TCB_ENABLE);	/* ENABLE, CLKDIV1 */
		check("TCB STATUS.RUN set when enabled",
			  !!(m->data[TB + TCBR_STATUS] & TCB_RUN), 1);

		/* Run until the first CAPT, recording when it fired. */
		long t0 = -1;
		for (int i = 0; i < 400 && t0 < 0; i++) {
			avr_run(m);
			if (m->data[TB + TCBR_INTFLAGS] & TCB_CAPT)
				t0 = (long)m->cycle;
		}
		check("first CAPT near 101 cycles", t0 >= 101 && t0 <= 104, 1);
		check("CAPT raises (enabled) interrupt", avr_has_pending_interrupts(m), 1);

		/* CNT keeps advancing; read mid-period returns a small live value. */
		uint8_t cnt = cpu_read(m, TB + TCBR_CNTL);
		check("CNT live read in range", cnt <= 100, 1);

		/* W1C the flag, then confirm the next CAPT ~101 cycles later (cadence). */
		cpu_write(m, TB + TCBR_INTFLAGS, TCB_CAPT);
		check("CAPT cleared by W1C", !!(m->data[TB + TCBR_INTFLAGS] & TCB_CAPT), 0);
		long t1 = -1;
		for (int i = 0; i < 400 && t1 < 0; i++) {
			avr_run(m);
			if (m->data[TB + TCBR_INTFLAGS] & TCB_CAPT)
				t1 = (long)m->cycle;
		}
		check("second CAPT ~101 cycles later", (t1 - t0) >= 99 && (t1 - t0) <= 104, 1);

		/* Disable: RUN clears and no further CAPT. */
		cpu_write(m, TB + TCBR_INTFLAGS, TCB_CAPT);	/* clear */
		cpu_write(m, TB + TCBR_CTRLA, 0x00);		/* disable */
		check("TCB STATUS.RUN clear when disabled",
			  !!(m->data[TB + TCBR_STATUS] & TCB_RUN), 0);
		int fired = 0;
		for (int i = 0; i < 400; i++) {
			avr_run(m);
			if (m->data[TB + TCBR_INTFLAGS] & TCB_CAPT) { fired = 1; break; }
		}
		check("no CAPT after disable", fired, 0);

		/* CLKDIV2 doubles the period (~202 cycles). */
		cpu_write(m, TB + TCBR_INTFLAGS, TCB_CAPT);
		cpu_write(m, TB + TCBR_CTRLA, TCB_ENABLE | TCB_CLKDIV2);
		long base = (long)m->cycle, t2 = -1;
		for (int i = 0; i < 600 && t2 < 0; i++) {
			avr_run(m);
			if (m->data[TB + TCBR_INTFLAGS] & TCB_CAPT)
				t2 = (long)m->cycle;
		}
		check("CLKDIV2 period ~202 cycles", (t2 - base) >= 200 && (t2 - base) <= 206, 1);
	}

	printf("== modern TCB0 8-bit PWM WO -> CCL (sim_tiny3217) ==\n");
	{
		const avr_io_addr_t TB = 0xa40, L = 0x1c0;
		enum { TCB_ENABLE = 0x01 };
		enum { CNTMODE_PWM8 = 0x07, CCMPEN = 0x10 };
		enum { TCB_CAPT = 0x01 };
		enum { CCL_EN = 0x01, LUT_EN = 0x01, CTRLA = 0x00,
			   L0CTRLB = 0x06, L0CTRLC = 0x07, TRUTH0 = 0x08 };

		avr_t *m = avr_make_mcu_by_name("attiny3217");
		if (!m) { printf("cannot make attiny3217 core\n"); return 2; }
		m->log = LOG_ERROR;
		avr_init(m);
		memset(m->flash, 0, 0x2000);

		uint8_t wo = 0xff, ccl0 = 0xff;
		avr_irq_register_notify(
			avr_io_getirq(m, AVR_IOCTL_TCB_GETIRQ('0'), AVR_TCB_IRQ_WO),
			rec_irq, &wo);
		/* LUT0 = pass-through of TCB0 WO (tinyAVR-1 INSEL 0x7, IN0). */
		avr_irq_register_notify(
			avr_io_getirq(m, AVR_IOCTL_CCL_GETIRQ('0'), AVR_CCL_IRQ_LUT0_OUT),
			rec_irq, &ccl0);
		cpu_write(m, L + L0CTRLB, 0x07);	/* IN0 = TCB0 WO */
		cpu_write(m, L + L0CTRLC, 0x00);
		cpu_write(m, L + TRUTH0, 0xaa);		/* OUT = IN0 */
		cpu_write(m, L + 0x05, LUT_EN);		/* L0CTRLA */
		cpu_write(m, L + CTRLA, CCL_EN);

		/* 8-bit PWM: CCMPL=99 (period 100), CCMPH=30 (high for 30 cycles). */
		cpu_write(m, TB + TCBR_CCMPL, 99);
		cpu_write(m, TB + TCBR_CCMPH, 30);
		cpu_write(m, TB + TCBR_CTRLB, CNTMODE_PWM8 | CCMPEN);
		long t0 = (long)m->cycle;
		cpu_write(m, TB + TCBR_CTRLA, TCB_ENABLE);

		check("WO set at BOTTOM when enabled", wo, 1);
		check("CCL LUT0 tracks WO high", ccl0, 1);

		#define RUN_TO(n) do { while ((long)m->cycle - t0 < (n)) avr_run(m); } while (0)
		RUN_TO(15);
		check("WO high before CCMPH", wo, 1);
		RUN_TO(35);
		check("WO cleared at CCMPH (30)", wo, 0);
		check("CCL LUT0 tracks WO low", ccl0, 0);
		RUN_TO(105);	/* past wrap at 100 */
		check("WO set again at next BOTTOM", wo, 1);

		/* CAPT flag fires once per period (at the period boundary). */
		cpu_write(m, TB + TCBR_INTFLAGS, TCB_CAPT);	/* clear */
		long tcapt = -1;
		for (int i = 0; i < 200 && tcapt < 0; i++) {
			avr_run(m);
			if (m->data[TB + TCBR_INTFLAGS] & TCB_CAPT) tcapt = (long)m->cycle;
		}
		check("PWM8 CAPT raised within a period", tcapt >= 0, 1);
		#undef RUN_TO

		/* CCMPH == BOTTOM => static low; CCMPH > TOP => static high. */
		cpu_write(m, TB + TCBR_CCMPH, 0);
		check("WO static low when CCMPH=BOTTOM", wo, 0);
		cpu_write(m, TB + TCBR_CCMPH, 200);
		check("WO static high when CCMPH>TOP", wo, 1);

		/* Clearing CCMPEN stops driving the output (low). */
		cpu_write(m, TB + TCBR_CTRLB, CNTMODE_PWM8);
		check("WO low when CCMPEN cleared", wo, 0);

		/* Disabling the timer drives WO low. */
		cpu_write(m, TB + TCBR_CTRLB, CNTMODE_PWM8 | CCMPEN);
		check("WO high again (CCMPH>TOP)", wo, 1);
		cpu_write(m, TB + TCBR_CTRLA, 0x00);
		check("WO low when TCB disabled", wo, 0);
	}

	printf("== modern TCA0 (sim_tiny3217) ==\n");
	{
		const avr_io_addr_t TA = 0xa00;
		enum { ENABLE = 0x01, DIV4 = (2 << 1) };	/* CLKSEL field 2 = /4 */
		enum { F_OVF = 0x01, F_CMP1 = 0x20 };

		avr_t *m = avr_make_mcu_by_name("attiny3217");
		if (!m) { printf("cannot make attiny3217 core\n"); return 2; }
		m->log = LOG_ERROR;
		avr_init(m);
		memset(m->flash, 0, 0x2000);	/* NOPs */

		/* PER (and PERBUF) reset to 0xFFFF, not 0 (DS40002205A 20.5.15). */
		check("TCA PER resets to 0xFFFF (low)", cpu_read(m, TA + TCAR_PERL), 0xff);
		check("TCA PER resets to 0xFFFF (high)", cpu_read(m, TA + TCAR_PERH), 0xff);
		check("TCA PERBUF resets to 0xFFFF (low)", cpu_read(m, TA + 0x36), 0xff);

		cpu_write(m, TA + TCAR_PERL, 200); cpu_write(m, TA + TCAR_PERH, 0);
		cpu_write(m, TA + TCAR_CMP1L, 50); cpu_write(m, TA + TCAR_CMP1L + 1, 0);
		cpu_write(m, TA + TCAR_INTCTRL, F_OVF | F_CMP1);	/* OVF + CMP1 enable */
		cpu_write(m, TA + TCAR_CTRLA, ENABLE);		/* DIV1 */

		/* CMP1 match at CNT == 50. */
		long tcmp = -1;
		for (int i = 0; i < 120 && tcmp < 0; i++) {
			avr_run(m);
			if (m->data[TA + TCAR_INTFLAGS] & F_CMP1) tcmp = (long)m->cycle;
		}
		check("CMP1 match near CNT=50", tcmp >= 50 && tcmp <= 53, 1);

		/* Overflow at TOP+1 == 201 cycles. */
		long tovf = -1;
		for (int i = 0; i < 250 && tovf < 0; i++) {
			avr_run(m);
			if (m->data[TA + TCAR_INTFLAGS] & F_OVF) tovf = (long)m->cycle;
		}
		check("OVF near 201 cycles", tovf >= 201 && tovf <= 205, 1);
		check("TCA interrupt pending", avr_has_pending_interrupts(m), 1);

		/* Live CNT read is in [0, PER]. */
		uint16_t cnt = cpu_read(m, TA + TCAR_CNTL);
		cnt |= cpu_read(m, TA + TCAR_CNTH) << 8;
		check("CNT live read <= PER", cnt <= 200, 1);

		/* W1C both flags. */
		cpu_write(m, TA + TCAR_INTFLAGS, F_OVF | F_CMP1);
		check("flags cleared by W1C",
			  m->data[TA + TCAR_INTFLAGS] & (F_OVF | F_CMP1), 0);

		/* Next overflow ~201 cycles after the previous one (cadence). */
		long t2 = -1;
		for (int i = 0; i < 260 && t2 < 0; i++) {
			avr_run(m);
			if (m->data[TA + TCAR_INTFLAGS] & F_OVF) t2 = (long)m->cycle;
		}
		check("OVF cadence ~201", (t2 - tovf) >= 199 && (t2 - tovf) <= 205, 1);

		/* Prescaler /4: overflow period becomes ~201*4 = 804 cycles. */
		cpu_write(m, TA + TCAR_INTFLAGS, F_OVF | F_CMP1);
		cpu_write(m, TA + TCAR_CTRLA, ENABLE | DIV4);
		long base = (long)m->cycle, t3 = -1;
		for (int i = 0; i < 1000 && t3 < 0; i++) {
			avr_run(m);
			if (m->data[TA + TCAR_INTFLAGS] & F_OVF) t3 = (long)m->cycle;
		}
		check("DIV4 overflow ~804 cycles", (t3 - base) >= 800 && (t3 - base) <= 810, 1);

		/* Disable: no further overflow. */
		cpu_write(m, TA + TCAR_INTFLAGS, F_OVF);
		cpu_write(m, TA + TCAR_CTRLA, 0x00);
		int fired = 0;
		for (int i = 0; i < 1000; i++) {
			avr_run(m);
			if (m->data[TA + TCAR_INTFLAGS] & F_OVF) { fired = 1; break; }
		}
		check("no OVF after disable", fired, 0);
	}

	printf("== modern TCA0 buffering / commands / FRQ / dual-slope / DIR / split (sim_tiny3217) ==\n");
	{
		const avr_io_addr_t TA = 0xa00;
		enum { ENABLE = 0x01, LUPD = 0x02, DIR = 0x01, CMD_UPDATE = 0x04,
			   CMD_RESTART = 0x08, CMD_RESET = 0x0c };
		enum { CMP0EN = 0x10, CMP1EN = 0x20, WG_FRQ = 0x01, WG_SS = 0x03,
			   WG_DSTOP = 0x05, WG_DSBOTH = 0x06, WG_DSBOTTOM = 0x07 };
		enum { CTRLECLR = 0x04, CTRLESET = 0x05, CTRLFCLR = 0x06, CTRLFSET = 0x07,
			   PERBUF = 0x36, CMP0BUF = 0x38, CMP1BUF = 0x3a };
		enum { F_OVF = 0x01, F_HUNF = 0x02, F_CMP0 = 0x10 };
		#define RUN_TO(mm, c) do { avr_cycle_count_t _end = (c); while ((mm)->cycle < _end) avr_run(mm); } while (0)
		#define W16(mm, a, v) do { cpu_write(mm, a, (v) & 0xff); cpu_write(mm, (a) + 1, (v) >> 8); } while (0)
		#define R16(mm, a) (cpu_read(mm, a) | (cpu_read(mm, (a) + 1) << 8))

		avr_t *m = avr_make_mcu_by_name("attiny3217");
		if (!m) { printf("cannot make attiny3217 core\n"); return 2; }
		m->log = LOG_ERROR;
		avr_init(m);
		memset(m->flash, 0, 0x2000);

		/* --- double buffering: PERBUF / CMPnBUF transfer on the wrap --- */
		W16(m, TA + TCAR_PERL, 99);
		W16(m, TA + TCAR_CMP0L, 30);
		cpu_write(m, TA + TCAR_CTRLB, WG_SS | CMP0EN);
		cpu_write(m, TA + TCAR_CTRLA, ENABLE);
		W16(m, TA + PERBUF, 49);
		W16(m, TA + CMP0BUF, 10);
		check("PERBV and CMP0BV set by buffer writes", cpu_read(m, TA + TCAR_CTRLFSET), 0x03);
		check("CTRLFCLR reads the same flags", cpu_read(m, TA + TCAR_CTRLFCLR), 0x03);
		check("PER unchanged before the update", R16(m, TA + TCAR_PERL), 99);
		RUN_TO(m, 110);
		check("PER loaded from PERBUF at the wrap", R16(m, TA + TCAR_PERL), 49);
		check("CMP0 loaded from CMP0BUF at the wrap", R16(m, TA + TCAR_CMP0L), 10);
		check("buffer valid flags cleared by the update", cpu_read(m, TA + TCAR_CTRLFSET), 0);

		/* LUPD blocks the transfer; the UPDATE command ignores it. */
		cpu_write(m, TA + CTRLESET, LUPD);
		check("LUPD reads back", cpu_read(m, TA + CTRLESET) & LUPD, LUPD);
		W16(m, TA + PERBUF, 29);
		RUN_TO(m, 300);
		check("LUPD keeps PER", R16(m, TA + TCAR_PERL), 49);
		check("PERBV stays set under LUPD", cpu_read(m, TA + TCAR_CTRLFSET), 0x01);
		cpu_write(m, TA + CTRLESET, CMD_UPDATE);
		check("UPDATE command loads PER despite LUPD", R16(m, TA + TCAR_PERL), 29);
		cpu_write(m, TA + CTRLESET, CMD_UPDATE);
		check("CMD bits read as 0", cpu_read(m, TA + CTRLESET) & 0x0c, 0);
		cpu_write(m, TA + CTRLECLR, LUPD);
		W16(m, TA + PERBUF, 19);
		cpu_write(m, TA + CTRLFCLR, 0x01);
		check("CTRLFCLR clears PERBV", cpu_read(m, TA + TCAR_CTRLFSET), 0);

		/* RESTART zeroes the counter; RESET needs the timer stopped. */
		RUN_TO(m, m->cycle + 7);
		cpu_write(m, TA + CTRLESET, CMD_RESTART);
		check("RESTART zeroes CNT", R16(m, TA + TCAR_CNTL) <= 1, 1);
		cpu_write(m, TA + CTRLESET, CMD_RESET);
		check("RESET ignored while enabled", cpu_read(m, TA + TCAR_CTRLB), WG_SS | CMP0EN);
		cpu_write(m, TA + TCAR_CTRLA, 0);
		cpu_write(m, TA + CTRLESET, CMD_RESET);
		check("RESET clears CTRLB", cpu_read(m, TA + TCAR_CTRLB), 0);
		check("RESET restores PER to 0xFFFF", R16(m, TA + TCAR_PERL), 0xffff);
		check("RESET restores PERBUF to 0xFFFF", R16(m, TA + PERBUF), 0xffff);

		/* --- CTRLC.CMPnOV drives WOn while the timer is stopped --- */
		uint8_t wo0 = 0xff, pin0 = 0xff;
		avr_irq_register_notify(avr_io_getirq(m, AVR_IOCTL_TCA_GETIRQ('0'), AVR_TCA_IRQ_WO0), rec_irq, &wo0);
		avr_irq_register_notify(avr_io_getirq(m, AVR_IOCTL_TCA_GETIRQ('0'), AVR_TCA_IRQ_PIN0), rec_irq, &pin0);
		cpu_write(m, TA + TCAR_CTRLB, WG_SS | CMP0EN);
		cpu_write(m, TA + TCAR_CTRLC, 0x01);
		check("CMP0OV drives WO0 while stopped", wo0, 1);
		check("pin overridden, level 1", pin0, 3);
		cpu_write(m, TA + TCAR_CTRLC, 0x00);
		check("CMP0OV clear drops WO0", wo0, 0);

		/* --- FRQ: WO0 toggles every CMP0+1 ticks --- */
		cpu_write(m, TA + TCAR_CTRLB, WG_FRQ | CMP0EN);
		W16(m, TA + TCAR_PERL, 0xffff);
		W16(m, TA + TCAR_CMP0L, 9);
		W16(m, TA + TCAR_CNTL, 0);
		avr_cycle_count_t t0 = m->cycle;
		cpu_write(m, TA + TCAR_CTRLA, ENABLE);
		int edges = 0; uint8_t last = wo0;
		avr_cycle_count_t e1 = 0, e2 = 0;
		while (m->cycle < t0 + 105) {
			avr_run(m);
			if (wo0 != last) {
				last = wo0;
				if (++edges == 1) e1 = m->cycle;
				if (edges == 2) e2 = m->cycle;
			}
		}
		check("FRQ toggles ~10 times in 105 cycles", edges >= 9 && edges <= 11, 1);
		check("FRQ half period ~10 cycles", (long)(e2 - e1) >= 9 && (long)(e2 - e1) <= 11, 1);
		cpu_write(m, TA + TCAR_CTRLA, 0);
		cpu_write(m, TA + CTRLESET, CMD_RESET);

		/* --- dual-slope: period 2*PER, OVF position per mode --- */
		for (int mode = 0; mode < 3; mode++) {
			static const uint8_t wg[3] = { WG_DSTOP, WG_DSBOTTOM, WG_DSBOTH };
			avr_t *d = avr_make_mcu_by_name("attiny3217");
			d->log = LOG_ERROR;
			avr_init(d);
			memset(d->flash, 0, 0x2000);
			wo0 = 0xff;
			avr_irq_register_notify(avr_io_getirq(d, AVR_IOCTL_TCA_GETIRQ('0'), AVR_TCA_IRQ_WO0), rec_irq, &wo0);
			W16(d, TA + TCAR_PERL, 20);
			W16(d, TA + TCAR_CMP0L, 5);
			cpu_write(d, TA + TCAR_CTRLB, wg[mode] | CMP0EN);
			cpu_write(d, TA + TCAR_CTRLA, ENABLE);
			avr_cycle_count_t s = d->cycle;
			long first_ovf = -1, second_ovf = -1, high = 0, ovfs = 0;
			uint32_t cmpmatches = 0; uint8_t prev = 0;
			while (d->cycle < s + 80) {
				avr_run(d);
				if (d->data[TA + TCAR_INTFLAGS] & F_OVF) {
					cpu_write(d, TA + TCAR_INTFLAGS, F_OVF);
					if (++ovfs == 1) first_ovf = d->cycle - s;
					if (ovfs == 2) second_ovf = d->cycle - s;
				}
				if (d->data[TA + TCAR_INTFLAGS] & F_CMP0) {
					cpu_write(d, TA + TCAR_INTFLAGS, F_CMP0);
					cmpmatches++;
				}
				if (d->cycle >= s + 40 && d->cycle < s + 80)
					high += wo0 == 1;
				(void)prev;
			}
			const char *nm[3] = { "DSTOP", "DSBOTTOM", "DSBOTH" };
			char buf[96];
			if (mode == 0) {
				snprintf(buf, sizeof buf, "%s first OVF at TOP (~20)", nm[mode]);
				check(buf, first_ovf >= 19 && first_ovf <= 23, 1);
				snprintf(buf, sizeof buf, "%s OVF cadence 40", nm[mode]);
				check(buf, second_ovf - first_ovf >= 39 && second_ovf - first_ovf <= 41, 1);
			} else if (mode == 1) {
				snprintf(buf, sizeof buf, "%s first OVF at BOTTOM (~40)", nm[mode]);
				check(buf, first_ovf >= 39 && first_ovf <= 43, 1);
			} else {
				snprintf(buf, sizeof buf, "%s OVF at both TOP and BOTTOM (2 in 45)", nm[mode]);
				check(buf, first_ovf >= 19 && first_ovf <= 23 && second_ovf >= 39 && second_ovf <= 43, 1);
			}
			snprintf(buf, sizeof buf, "%s WO0 duty = 10 of 40 ticks", nm[mode]);
			check(buf, high >= 9 && high <= 11, 1);
			snprintf(buf, sizeof buf, "%s CMP0 matches up and down (>=2 in 80)", nm[mode]);
			check(buf, cmpmatches >= 2, 1);
		}

		/* --- software DIR: counter counts down and reloads PER --- */
		{
			avr_t *d = avr_make_mcu_by_name("attiny3217");
			d->log = LOG_ERROR;
			avr_init(d);
			memset(d->flash, 0, 0x2000);
			W16(d, TA + TCAR_PERL, 99);
			W16(d, TA + TCAR_CNTL, 50);
			cpu_write(d, TA + CTRLESET, DIR);
			cpu_write(d, TA + TCAR_CTRLA, ENABLE);
			RUN_TO(d, 11);
			long c = R16(d, TA + TCAR_CNTL);
			check("DIR=1 counts down (~40 after 10 ticks)", c >= 38 && c <= 41, 1);
			check("DIR reads back 1", cpu_read(d, TA + CTRLESET) & DIR, DIR);
			RUN_TO(d, 70);
			c = R16(d, TA + TCAR_CNTL);
			check("down-count reloads PER at BOTTOM", c > 70 && c <= 99, 1);
			check("underflow raised OVF", d->data[TA + TCAR_INTFLAGS] & F_OVF, F_OVF);
		}

		/* --- EVACT UPDOWN: event level high counts down --- */
		{
			avr_t *d = avr_make_mcu_by_name("attiny3217");
			d->log = LOG_ERROR;
			avr_init(d);
			memset(d->flash, 0, 0x2000);
			avr_irq_t *ev = avr_io_getirq(d, AVR_IOCTL_TCA_GETIRQ('0'), AVR_TCA_IRQ_EV_IN);
			W16(d, TA + TCAR_PERL, 999);
			W16(d, TA + TCAR_CNTL, 100);
			cpu_write(d, TA + TCAR_EVCTRL, 0x01 | (3 << 1));	/* CNTEI, UPDOWN */
			cpu_write(d, TA + TCAR_CTRLA, ENABLE);
			RUN_TO(d, 21);
			long up = R16(d, TA + TCAR_CNTL);
			check("UPDOWN counts up while event low", up >= 118 && up <= 122, 1);
			avr_raise_irq(ev, 1);
			RUN_TO(d, 41);
			long dn = R16(d, TA + TCAR_CNTL);
			check("UPDOWN counts down while event high", dn >= 98 && dn <= 102, 1);
			avr_raise_irq(ev, 0);
			RUN_TO(d, 51);
			long up2 = R16(d, TA + TCAR_CNTL);
			check("UPDOWN resumes up when event drops", up2 > dn, 1);
		}

		/* --- CNT written while running takes effect --- */
		{
			avr_t *d = avr_make_mcu_by_name("attiny3217");
			d->log = LOG_ERROR;
			avr_init(d);
			memset(d->flash, 0, 0x2000);
			W16(d, TA + TCAR_PERL, 999);
			cpu_write(d, TA + TCAR_CTRLA, ENABLE);
			RUN_TO(d, 20);
			W16(d, TA + TCAR_CNTL, 500);
			RUN_TO(d, 30);
			long c = R16(d, TA + TCAR_CNTL);
			check("CNT write while running re-anchors the count", c >= 505 && c <= 512, 1);
		}

		/* --- split mode: two independent 8-bit down-counters, WO0-5 --- */
		{
			avr_t *d = avr_make_mcu_by_name("attiny3217");
			d->log = LOG_ERROR;
			avr_init(d);
			memset(d->flash, 0, 0x2000);
			uint8_t lwo0 = 0xff, hwo0 = 0xff, hpin = 0xff;
			avr_irq_register_notify(avr_io_getirq(d, AVR_IOCTL_TCA_GETIRQ('0'), AVR_TCA_IRQ_WO0), rec_irq, &lwo0);
			avr_irq_register_notify(avr_io_getirq(d, AVR_IOCTL_TCA_GETIRQ('0'), AVR_TCA_IRQ_WO3), rec_irq, &hwo0);
			avr_irq_register_notify(avr_io_getirq(d, AVR_IOCTL_TCA_GETIRQ('0'), AVR_TCA_IRQ_PIN3), rec_irq, &hpin);
			cpu_write(d, TA + 0x03, 0x01);		/* CTRLD.SPLITM */
			check("LPER resets to 0xFF", cpu_read(d, TA + 0x26), 0xff);
			cpu_write(d, TA + 0x26, 99);		/* LPER */
			cpu_write(d, TA + 0x27, 49);		/* HPER */
			cpu_write(d, TA + 0x28, 30);		/* LCMP0 */
			cpu_write(d, TA + 0x29, 10);		/* HCMP0 */
			cpu_write(d, TA + 0x0a, 0x13);		/* LUNF | HUNF | LCMP0 enable */
			cpu_write(d, TA + TCAR_CTRLB, 0x01 | 0x10);	/* LCMP0EN | HCMP0EN */
			check("HCMP0EN overrides the WO3 pin", hpin & 2, 2);
			avr_cycle_count_t s = d->cycle;
			cpu_write(d, TA + TCAR_CTRLA, ENABLE);
			long lhigh = 0, hhigh = 0, hunf = -1, lunf = -1;
			while (d->cycle < s + 100) {
				avr_run(d);
				lhigh += lwo0 == 1;
				hhigh += hwo0 == 1;
				if (hunf < 0 && (d->data[TA + TCAR_INTFLAGS] & F_HUNF)) hunf = d->cycle - s;
				if (lunf < 0 && (d->data[TA + TCAR_INTFLAGS] & F_OVF)) lunf = d->cycle - s;
			}
			long lcnt = cpu_read(d, TA + 0x20), hcnt = cpu_read(d, TA + 0x21);
			check("HUNF fires at the high-byte BOTTOM (~49)", hunf >= 48 && hunf <= 52, 1);
			check("LUNF fires at the low-byte BOTTOM (~99)", lunf >= 98 && lunf <= 102, 1);
			check("low timer WO0 duty ~30 of 100", lhigh >= 28 && lhigh <= 32, 1);
			check("high timer WO3 duty ~10 of 50 (2 periods)", hhigh >= 19 && hhigh <= 23, 1);
			check("LCNT within 0..LPER", lcnt <= 99, 1);
			check("HCNT within 0..HPER", hcnt <= 49, 1);
			check("LCMP0 flag set", d->data[TA + TCAR_INTFLAGS] & F_CMP0, F_CMP0);
			cpu_write(d, TA + TCAR_INTFLAGS, 0x73);
			check("split flags clear by W1C", d->data[TA + TCAR_INTFLAGS], 0);
			/* HUNF uses the vector after OVF. */
			RUN_TO(d, d->cycle + 60);
			check("HUNF interrupt pending", avr_has_pending_interrupts(d), 1);
			/* Split-mode RESET (both halves) returns to normal mode. */
			cpu_write(d, TA + TCAR_CTRLA, 0);
			cpu_write(d, TA + CTRLESET, CMD_RESET | 0x03);
			check("split RESET clears SPLITM", cpu_read(d, TA + 0x03), 0);
		}
		#undef RUN_TO
		#undef W16
		#undef R16
	}

	printf("== modern TCA0 single-slope PWM WO -> CCL (sim_tiny3217) ==\n");
	{
		const avr_io_addr_t TA = 0xa00, L = 0x1c0;
		enum { ENABLE = 0x01 };
		enum { WGMODE_SS = 0x03, WGMODE_FRQ = 0x01,
			   CMP0EN = 0x10, CMP1EN = 0x20, CMP2EN = 0x40 };	/* CTRLB bits 4/5/6 */
		enum { CCL_EN = 0x01, LUT_EN = 0x01, CTRLA = 0x00,
			   L0CTRLA = 0x05, L0CTRLB = 0x06, L0CTRLC = 0x07, TRUTH0 = 0x08 };

		avr_t *m = avr_make_mcu_by_name("attiny3217");
		if (!m) { printf("cannot make attiny3217 core\n"); return 2; }
		m->log = LOG_ERROR;
		avr_init(m);
		memset(m->flash, 0, 0x2000);

		uint8_t wo0 = 0xff, ccl0 = 0xff;
		avr_irq_register_notify(
			avr_io_getirq(m, AVR_IOCTL_TCA_GETIRQ('0'), AVR_TCA_IRQ_WO0),
			rec_irq, &wo0);

		/* LUT0 = pass-through of TCA0 WO0 (tinyAVR-1 INSEL 0x8 -> WOn on INn);
		 * the core wires TCA0 WO0 to the CCL source, so the LUT tracks it. */
		avr_irq_register_notify(
			avr_io_getirq(m, AVR_IOCTL_CCL_GETIRQ('0'), AVR_CCL_IRQ_LUT0_OUT),
			rec_irq, &ccl0);
		cpu_write(m, L + L0CTRLB, 0x08);	/* IN0 = TCA0 WO0 */
		cpu_write(m, L + L0CTRLC, 0x00);	/* IN2 mask */
		cpu_write(m, L + TRUTH0, 0xaa);		/* OUT = IN0 */
		cpu_write(m, L + L0CTRLA, LUT_EN);
		cpu_write(m, L + CTRLA, CCL_EN);

		/* Single-slope PWM: PER=99 (period 100), CMP0=30 duty cycle. */
		cpu_write(m, TA + TCAR_PERL, 99); cpu_write(m, TA + TCAR_PERH, 0);
		cpu_write(m, TA + TCAR_CMP0L, 30); cpu_write(m, TA + TCAR_CMP0L + 1, 0);
		cpu_write(m, TA + TCAR_CTRLB, WGMODE_SS | CMP0EN);
		long t0 = (long)m->cycle;
		cpu_write(m, TA + TCAR_CTRLA, ENABLE);

		check("WO0 set at BOTTOM when enabled", wo0, 1);
		check("CCL LUT0 tracks WO0 high", ccl0, 1);

		#define RUN_TO(n) do { while ((long)m->cycle - t0 < (n)) avr_run(m); } while (0)
		RUN_TO(15);
		check("WO0 high before CMP0 match", wo0, 1);
		RUN_TO(35);
		check("WO0 cleared on CMP0 match (30)", wo0, 0);
		check("CCL LUT0 tracks WO0 low", ccl0, 0);
		RUN_TO(105);	/* past wrap at 100 */
		check("WO0 set again at next BOTTOM", wo0, 1);
		#undef RUN_TO

		/* CMP0 == BOTTOM => static low; CMP0 > TOP => static high. */
		cpu_write(m, TA + TCAR_CMP0L, 0); cpu_write(m, TA + TCAR_CMP0L + 1, 0);
		check("WO0 static low when CMP0=BOTTOM", wo0, 0);
		cpu_write(m, TA + TCAR_CMP0L, 200); cpu_write(m, TA + TCAR_CMP0L + 1, 0);
		check("WO0 static high when CMP0>TOP", wo0, 1);

		/* Clearing CMP0EN stops overriding the pin (low). */
		cpu_write(m, TA + TCAR_CTRLB, WGMODE_SS);
		check("WO0 low when CMP0EN cleared", wo0, 0);

		/* Discriminating check (CTRLB bit positions): WO0 is gated by CMP0EN
		 * (bit 4 = 0x10), NOT by CMP1EN (bit 5 = 0x20). Enabling only CMP1EN
		 * must leave WO0 low. CMP0 is 30 (< TOP), so a wrong bit map would drive
		 * WO0 high here. */
		cpu_write(m, TA + TCAR_CMP0L, 30); cpu_write(m, TA + TCAR_CMP0L + 1, 0);
		cpu_write(m, TA + TCAR_CTRLB, WGMODE_SS | CMP1EN);
		check("WO0 NOT driven by CMP1EN (bit map correct)", wo0, 0);
		cpu_write(m, TA + TCAR_CTRLB, WGMODE_SS | CMP0EN);
		check("WO0 driven by CMP0EN=0x10", wo0, 1);

		/* FRQ / dual-slope WGMODE variants are unmodelled => WO stays low. */
		cpu_write(m, TA + TCAR_CMP0L, 30); cpu_write(m, TA + TCAR_CMP0L + 1, 0);
		cpu_write(m, TA + TCAR_CTRLB, WGMODE_FRQ | CMP0EN);
		check("WO0 low in unmodelled FRQ mode", wo0, 0);

		/* Disabling the timer drives every WO low. */
		cpu_write(m, TA + TCAR_CTRLB, WGMODE_SS | CMP0EN);
		cpu_write(m, TA + TCAR_CMP0L, 200);
		check("WO0 high again (CMP0>TOP, single-slope)", wo0, 1);
		cpu_write(m, TA + TCAR_CTRLA, 0x00);
		check("WO0 low when TCA disabled", wo0, 0);
	}

	printf("== modern TCA0 event counting (EVCTRL.CNTEI, sim_tiny3217) ==\n");
	{
		const avr_io_addr_t TA = 0xa00, E = 0x180;
		enum { ENABLE = 0x01 };
		enum { CNTEI = 0x01, EV_POSEDGE = (0 << 1), EV_ANYEDGE = (1 << 1),
			   EV_HIGHLVL = (2 << 1) };
		enum { F_OVF = 0x01, F_CMP1 = 0x20, IE_OVF = 0x01, IE_CMP1 = 0x20 };
		enum { SYNCUSER0 = 0x22, SEL_SYNCCH0 = 1 };

		avr_t *m = avr_make_mcu_by_name("attiny3217");
		if (!m) { printf("cannot make attiny3217 core\n"); return 2; }
		m->log = LOG_ERROR;
		avr_init(m);
		memset(m->flash, 0, 0x2000);	/* NOPs */

		/* Route EVSYS SYNCUSER0 (= TCA0) to SYNCCH0; the core wires that user's
		 * output to TCA0's EV_IN, so driving the channel drives the event line. */
		cpu_write(m, E + SYNCUSER0, SEL_SYNCCH0);
		avr_irq_t *ch0 = avr_io_getirq(m, AVR_IOCTL_EVSYS_GETIRQ('0'),
									   AVR_EVSYS_IRQ_CH0 + 0);

		/* POSEDGE event count: PER=5 (wrap after 6 counts), CMP1 at 3. The
		 * prescaled clock must not advance CNT in this mode. */
		cpu_write(m, TA + TCAR_PERL, 5); cpu_write(m, TA + TCAR_PERH, 0);
		cpu_write(m, TA + TCAR_CMP1L, 3); cpu_write(m, TA + TCAR_CMP1L + 1, 0);
		cpu_write(m, TA + TCAR_INTCTRL, IE_OVF | IE_CMP1);
		cpu_write(m, TA + TCAR_EVCTRL, CNTEI | EV_POSEDGE);
		cpu_write(m, TA + TCAR_CTRLA, ENABLE);

		long base = (long)m->cycle;
		while ((long)m->cycle - base < 200) avr_run(m);
		uint16_t cnt = cpu_read(m, TA + TCAR_CNTL);
		cnt |= cpu_read(m, TA + TCAR_CNTH) << 8;
		check("event-count: clock idle, CNT stays 0", cnt, 0);

		/* Each rising edge advances CNT by one; falling edges are ignored. */
		#define PULSE() do { avr_raise_irq(ch0, 1); avr_raise_irq(ch0, 0); } while (0)
		PULSE();
		cnt = cpu_read(m, TA + TCAR_CNTL); cnt |= cpu_read(m, TA + TCAR_CNTH) << 8;
		check("POSEDGE edge 1 -> CNT=1", cnt, 1);
		PULSE(); PULSE();		/* CNT = 3 -> CMP1 match */
		cnt = cpu_read(m, TA + TCAR_CNTL); cnt |= cpu_read(m, TA + TCAR_CNTH) << 8;
		check("POSEDGE edge 3 -> CNT=3", cnt, 3);
		check("CMP1 match at CNT=3", !!(m->data[TA + TCAR_INTFLAGS] & F_CMP1), 1);
		PULSE(); PULSE();		/* CNT = 5 (== TOP), not yet wrapped */
		check("no OVF before wrap", !!(m->data[TA + TCAR_INTFLAGS] & F_OVF), 0);
		PULSE();			/* 6th edge: 5 -> 0 wrap, OVF */
		cnt = cpu_read(m, TA + TCAR_CNTL); cnt |= cpu_read(m, TA + TCAR_CNTH) << 8;
		check("POSEDGE wrap -> CNT=0", cnt, 0);
		check("OVF on wrap", !!(m->data[TA + TCAR_INTFLAGS] & F_OVF), 1);

		/* ANYEDGE: both edges tick (CNT currently 0). */
		cpu_write(m, TA + TCAR_INTFLAGS, F_OVF | F_CMP1);
		cpu_write(m, TA + TCAR_EVCTRL, CNTEI | EV_ANYEDGE);
		avr_raise_irq(ch0, 1);		/* rising  -> CNT=1 */
		avr_raise_irq(ch0, 0);		/* falling -> CNT=2 */
		cnt = cpu_read(m, TA + TCAR_CNTL); cnt |= cpu_read(m, TA + TCAR_CNTH) << 8;
		check("ANYEDGE counts both edges -> CNT=2", cnt, 2);
		#undef PULSE

		/* HIGHLVL: the prescaled clock runs only while the event line is high. */
		cpu_write(m, TA + TCAR_CTRLA, 0x00);		/* disable to reconfigure */
		cpu_write(m, TA + TCAR_PERL, 200); cpu_write(m, TA + TCAR_PERH, 0);
		cpu_write(m, TA + TCAR_CNTL, 0); cpu_write(m, TA + TCAR_CNTH, 0);
		cpu_write(m, TA + TCAR_EVCTRL, CNTEI | EV_HIGHLVL);
		avr_raise_irq(ch0, 0);				/* event line low */
		cpu_write(m, TA + TCAR_CTRLA, ENABLE);		/* DIV1, but gated off */

		base = (long)m->cycle;
		while ((long)m->cycle - base < 100) avr_run(m);
		cnt = cpu_read(m, TA + TCAR_CNTL); cnt |= cpu_read(m, TA + TCAR_CNTH) << 8;
		check("HIGHLVL gated off: CNT held at 0", cnt, 0);

		avr_raise_irq(ch0, 1);				/* gate opens: clock runs */
		long t1 = (long)m->cycle;
		while ((long)m->cycle - t1 < 50) avr_run(m);
		cnt = cpu_read(m, TA + TCAR_CNTL); cnt |= cpu_read(m, TA + TCAR_CNTH) << 8;
		check("HIGHLVL gate open: CNT advanced ~50", cnt >= 48 && cnt <= 52, 1);

		avr_raise_irq(ch0, 0);				/* gate closes: freeze */
		uint16_t frozen = cpu_read(m, TA + TCAR_CNTL);
		frozen |= cpu_read(m, TA + TCAR_CNTH) << 8;
		long t2 = (long)m->cycle;
		while ((long)m->cycle - t2 < 100) avr_run(m);
		cnt = cpu_read(m, TA + TCAR_CNTL); cnt |= cpu_read(m, TA + TCAR_CNTH) << 8;
		check("HIGHLVL gate closed: CNT frozen", cnt, frozen);
	}

	printf("== modern USART0 (sim_tiny3217) ==\n");
	{
		const avr_io_addr_t U = 0x800;
		enum { RXDATAL = 0x00, TXDATAL = 0x02, STATUS = 0x04,
			   CTRLA = 0x05, CTRLB = 0x06, BAUDL = 0x08, BAUDH = 0x09 };
		enum { RXCIF = 0x80, TXCIF = 0x40, DREIF = 0x20 };
		enum { RXCIE = 0x80, DREIE = 0x20 };
		enum { RXEN = 0x80, TXEN = 0x40 };

		avr_t *m = avr_make_mcu_by_name("attiny3217");
		if (!m) { printf("cannot make attiny3217 core\n"); return 2; }
		m->log = LOG_ERROR;
		avr_init(m);
		memset(m->flash, 0, 0x2000);	/* NOPs */
		uint32_t ioctl = AVR_IOCTL_UART_GETIRQ('0');

		check("DREIF set at reset", !!(m->data[U + STATUS] & DREIF), 1);

		/* ---- transmit ---- */
		g_irqval = 0;
		avr_irq_register_notify(avr_io_getirq(m, ioctl, UART_IRQ_OUTPUT),
								rec_irq, &g_irqval);
		cpu_write(m, U + BAUDL, 64); cpu_write(m, U + BAUDH, 0);	/* frame ~160 cy */
		cpu_write(m, U + CTRLB, TXEN);
		cpu_write(m, U + TXDATAL, 'A');
		check("TX byte emitted on OUTPUT IRQ", g_irqval, 'A');
		check("DREIF still set after TX", !!(m->data[U + STATUS] & DREIF), 1);
		check("TXCIF not set immediately", !!(m->data[U + STATUS] & TXCIF), 0);

		/* TXC asserts after the frame time. */
		int txc = 0;
		for (int i = 0; i < 400; i++) {
			avr_run(m);
			if (m->data[U + STATUS] & TXCIF) { txc = 1; break; }
		}
		check("TXCIF set after frame", txc, 1);
		cpu_write(m, U + STATUS, TXCIF);	/* W1C */
		check("TXCIF cleared by W1C", !!(m->data[U + STATUS] & TXCIF), 0);

		/* ---- receive ---- */
		cpu_write(m, U + CTRLB, TXEN | RXEN);
		avr_irq_t *in = avr_io_getirq(m, ioctl, UART_IRQ_INPUT);
		avr_raise_irq(in, 'Z');
		check("RXCIF set on input", !!(m->data[U + STATUS] & RXCIF), 1);
		check("RXDATAL returns the byte", cpu_read(m, U + RXDATAL), 'Z');
		check("RXCIF cleared when fifo empty",
			  !!(m->data[U + STATUS] & RXCIF), 0);

		/* fifo holds multiple bytes in order */
		avr_raise_irq(in, 'h');
		avr_raise_irq(in, 'i');
		check("fifo byte 1", cpu_read(m, U + RXDATAL), 'h');
		check("RXCIF still set (1 left)", !!(m->data[U + STATUS] & RXCIF), 1);
		check("fifo byte 2", cpu_read(m, U + RXDATAL), 'i');
		check("RXCIF clear after draining", !!(m->data[U + STATUS] & RXCIF), 0);

		/* RXC interrupt fires when enabled */
		cpu_write(m, U + CTRLA, RXCIE);
		avr_raise_irq(in, 'q');
		check("RXC interrupt pending", avr_has_pending_interrupts(m), 1);

		/* receive ignored while RXEN is off */
		cpu_read(m, U + RXDATAL);		/* drain */
		cpu_write(m, U + CTRLA, 0);
		cpu_write(m, U + CTRLB, 0);		/* RXEN off */
		avr_raise_irq(in, 'x');
		check("input ignored with RXEN=0", !!(m->data[U + STATUS] & RXCIF), 0);
	}

	printf("== modern NVMCTRL / EEPROM (sim_tiny3217) ==\n");
	{
		const avr_io_addr_t NV = 0x1000, EE = 0x1400;
		enum { CTRLA = 0x00, STATUS = 0x02, INTCTRL = 0x03, INTFLAGS = 0x04 };
		enum { CMD_PAGEWRITE = 1, CMD_PAGEERASEWRITE = 3, CMD_PAGEBUFCLR = 4,
			   CMD_EEERASE = 6 };
		enum { EEREADY = 0x01 };

		enum { EEBUSY = 0x02 };

		avr_t *m = avr_make_mcu_by_name("attiny3217");
		if (!m) { printf("cannot make attiny3217 core\n"); return 2; }
		m->log = LOG_ERROR;
		avr_init(m);
		memset(m->flash, 0, 0x2000);	/* NOPs, so avr_run advances cleanly */

		/* Run out the nominal NVM busy phase (operations are no longer instant). */
		#define NVM_RUN() do { for (int i = 0; i < 200; i++) avr_run(m); } while (0)

		check("erased EEPROM reads 0xFF", cpu_read(m, EE + 5), 0xff);

		/* A write loads the page buffer but does NOT commit. */
		cpu_write(m, EE + 5, 0xab);
		check("EEPROM unchanged before command", cpu_read(m, EE + 5), 0xff);

		/* Command ignored without an open CCP window. */
		m->arch.ccp_window = 0;
		cpu_write(m, NV + CTRLA, CMD_PAGEERASEWRITE);
		check("commit ignored without CCP", cpu_read(m, EE + 5), 0xff);

		/* With CCP, ERASEWRITE commits the buffered byte immediately, then the
		 * controller is briefly EEBUSY before EEREADY is flagged. */
		avr_ccp_write(m, AVR_CCP_IOREG);
		cpu_write(m, NV + CTRLA, CMD_PAGEERASEWRITE);
		check("ERASEWRITE commits byte", cpu_read(m, EE + 5), 0xab);
		check("EEBUSY set during op", !!(m->data[NV + STATUS] & EEBUSY), 1);
		check("EEREADY not yet set while busy",
			  !!(m->data[NV + INTFLAGS] & EEREADY), 0);
		NVM_RUN();
		check("EEBUSY clears after op", !!(m->data[NV + STATUS] & EEBUSY), 0);
		check("EEREADY set on completion",
			  !!(m->data[NV + INTFLAGS] & EEREADY), 1);
		cpu_write(m, NV + INTFLAGS, EEREADY);	/* clear for later */

		/* Multiple bytes, in place. */
		cpu_write(m, EE + 0, 0x11);
		cpu_write(m, EE + 1, 0x22);
		avr_ccp_write(m, AVR_CCP_IOREG);
		cpu_write(m, NV + CTRLA, CMD_PAGEWRITE);
		check("byte 0 committed", cpu_read(m, EE + 0), 0x11);
		check("byte 1 committed", cpu_read(m, EE + 1), 0x22);
		check("untouched byte preserved", cpu_read(m, EE + 5), 0xab);
		NVM_RUN();

		/* PAGEBUFCLR aborts a pending buffer load. */
		cpu_write(m, EE + 0, 0x99);
		avr_ccp_write(m, AVR_CCP_IOREG);
		cpu_write(m, NV + CTRLA, CMD_PAGEBUFCLR);
		NVM_RUN();
		avr_ccp_write(m, AVR_CCP_IOREG);
		cpu_write(m, NV + CTRLA, CMD_PAGEWRITE);
		check("PAGEBUFCLR discarded the write", cpu_read(m, EE + 0), 0x11);
		NVM_RUN();

		/* EE interrupt fires when enabled — after the busy phase completes. */
		cpu_write(m, NV + INTFLAGS, EEREADY);	/* clear */
		cpu_write(m, NV + INTCTRL, EEREADY);	/* enable */
		cpu_write(m, EE + 2, 0x33);
		avr_ccp_write(m, AVR_CCP_IOREG);
		cpu_write(m, NV + CTRLA, CMD_PAGEWRITE);
		NVM_RUN();
		check("EE interrupt pending", avr_has_pending_interrupts(m), 1);
		cpu_write(m, NV + INTFLAGS, EEREADY);	/* W1C */
		check("EEREADY cleared by W1C", !!(m->data[NV + INTFLAGS] & EEREADY), 0);

		/* EEERASE wipes the whole EEPROM. */
		avr_ccp_write(m, AVR_CCP_IOREG);
		cpu_write(m, NV + CTRLA, CMD_EEERASE);
		check("EEERASE wipes byte 0", cpu_read(m, EE + 0), 0xff);
		check("EEERASE wipes byte 5", cpu_read(m, EE + 5), 0xff);
		NVM_RUN();

		/* Regression: committed EEPROM is persistent across a reset, and a
		 * committed 0x00 byte is preserved (not promoted to 0xFF). Both were
		 * broken when the mapped EEPROM sat inside avr_reset()'s clear range. */
		cpu_write(m, EE + 0, 0x5a);
		cpu_write(m, EE + 1, 0x00);		/* a legitimate zero byte */
		avr_ccp_write(m, AVR_CCP_IOREG);
		cpu_write(m, NV + CTRLA, CMD_PAGEERASEWRITE);
		NVM_RUN();
		check("EEPROM committed before reset", cpu_read(m, EE + 0), 0x5a);
		avr_reset(m);
		check("EEPROM persists across reset", cpu_read(m, EE + 0), 0x5a);
		check("committed 0x00 byte not corrupted after reset",
			  cpu_read(m, EE + 1), 0x00);

		#undef NVM_RUN
	}

	printf("== modern NVMCTRL / USERROW (sim_tiny3217) ==\n");
	{
		const avr_io_addr_t NV = 0x1000, EE = 0x1400, URO = 0x1300;
		enum { CTRLA = 0x00, STATUS = 0x02 };
		enum { CMD_PAGEWRITE = 1, CMD_PAGEERASE = 2, CMD_PAGEERASEWRITE = 3,
			   CMD_CHIPERASE = 5 };
		enum { EEBUSY = 0x02 };

		avr_t *m = avr_make_mcu_by_name("attiny3217");
		if (!m) { printf("cannot make attiny3217 core\n"); return 2; }
		m->log = LOG_ERROR;
		avr_init(m);
		memset(m->flash, 0, 0x2000);
		#define NVM_RUN() do { for (int i = 0; i < 200; i++) avr_run(m); } while (0)

		/* USERROW is "one extra page of EEPROM" (DS40002205A 6.6): erased = 0xFF,
		 * written via the EEPROM page-buffer + commands, persistent, and not
		 * affected by chip erase. */
		check("erased USERROW reads 0xFF", cpu_read(m, URO + 3), 0xff);

		/* A write loads the buffer but does not commit. */
		cpu_write(m, URO + 3, 0x5a);
		check("USERROW unchanged before command", cpu_read(m, URO + 3), 0xff);

		/* CCP + PAGEWRITE commits to USERROW (EEBUSY like EEPROM). */
		avr_ccp_write(m, AVR_CCP_IOREG);
		cpu_write(m, NV + CTRLA, CMD_PAGEWRITE);
		check("USERROW PAGEWRITE commits byte", cpu_read(m, URO + 3), 0x5a);
		check("USERROW op asserts EEBUSY", !!(m->data[NV + STATUS] & EEBUSY), 1);
		NVM_RUN();

		/* USERROW and EEPROM are independent regions. */
		cpu_write(m, EE + 3, 0x11);
		avr_ccp_write(m, AVR_CCP_IOREG);
		cpu_write(m, NV + CTRLA, CMD_PAGEERASEWRITE);	/* last write = EEPROM */
		NVM_RUN();
		check("EEPROM write did not disturb USERROW", cpu_read(m, URO + 3), 0x5a);

		/* CHIPERASE wipes EEPROM but NOT USERROW (DS40002205A 6.6). */
		avr_ccp_write(m, AVR_CCP_IOREG);
		cpu_write(m, NV + CTRLA, CMD_CHIPERASE);
		NVM_RUN();
		check("CHIPERASE wipes EEPROM", cpu_read(m, EE + 3), 0xff);
		check("CHIPERASE leaves USERROW intact", cpu_read(m, URO + 3), 0x5a);

		/* USERROW persists across reset. */
		avr_reset(m);
		check("USERROW persists across reset", cpu_read(m, URO + 3), 0x5a);

		/* PAGEERASE sets the buffered USERROW bytes back to 0xFF. */
		cpu_write(m, URO + 3, 0x00);	/* load buffer (value irrelevant for erase) */
		avr_ccp_write(m, AVR_CCP_IOREG);
		cpu_write(m, NV + CTRLA, CMD_PAGEERASE);
		check("USERROW PAGEERASE -> 0xFF", cpu_read(m, URO + 3), 0xff);
		NVM_RUN();

		#undef NVM_RUN
	}

	printf("== modern NVMCTRL flash self-programming (sim_tiny3217) ==\n");
	{
		const avr_io_addr_t NV = 0x1000, EE = 0x1400;
		enum { CTRLA = 0x00 };
		enum { CMD_PAGEWRITE = 1, CMD_PAGEERASE = 2, CMD_PAGEBUFCLR = 4 };
		/* Flash is mapped at 0x8000; target page 2 (offset 0x100) to avoid the
		 * program at flash 0. Self-programming stores go through ST/STS, which
		 * the engine routes to the NVM page buffer (cpu_write would bypass it). */
		const uint16_t FA = 0x8100;	/* data address of flash offset 0x100 */
		const uint32_t FO = 0x100;	/* flash[] offset it maps to */

		avr_t *m = avr_make_mcu_by_name("attiny3217");
		if (!m) { printf("cannot make attiny3217 core\n"); return 2; }
		m->log = LOG_ERROR;
		avr_init(m);
		memset(m->flash, 0, 0x2000);

		/* Program: LDI r16,0xAB; STS 0x8100,r16; LDI r16,0xCD; STS 0x8101,r16 */
		put16(m, 0x00, 0xea0b);			/* LDI r16, 0xAB */
		put16(m, 0x02, 0x9300); put16(m, 0x04, FA);	/* STS 0x8100, r16 */
		put16(m, 0x06, 0xec0d);			/* LDI r16, 0xCD */
		put16(m, 0x08, 0x9300); put16(m, 0x0a, FA + 1);	/* STS 0x8101, r16 */
		m->pc = 0;
		for (int i = 0; i < 4; i++) step(m);	/* run the four instructions */

		/* The stores loaded the page buffer but did not touch flash. */
		check("flash unchanged before command", m->flash[FO], 0x00);

		/* A page command without an open CCP window is ignored. */
		cpu_write(m, NV + CTRLA, CMD_PAGEWRITE);
		check("flash commit ignored without CCP", m->flash[FO], 0x00);

		/* PAGEWRITE commits the buffered bytes into flash. */
		avr_ccp_write(m, AVR_CCP_IOREG);
		cpu_write(m, NV + CTRLA, CMD_PAGEWRITE);
		check("flash byte 0 written (0xAB)", m->flash[FO], 0xab);
		check("flash byte 1 written (0xCD)", m->flash[FO + 1], 0xcd);
		check("flash neighbour untouched", m->flash[FO + 2], 0x00);
		check("FBUSY set during flash op", !!(m->data[NV + 0x02] & 0x01), 1);

		/* The committed byte is visible through the mapped-flash read path. */
		m->pc = 0;
		put16(m, 0x00, 0x9000); put16(m, 0x02, FA);	/* LDS r0, 0x8100 */
		m->data[0] = 0;
		step(m);
		check("mapped-flash read sees commit", m->data[0], 0xab);

		/* PAGEERASE erases the addressed page to 0xFF. */
		avr_ccp_write(m, AVR_CCP_IOREG);
		cpu_write(m, NV + CTRLA, CMD_PAGEERASE);
		check("flash page erased to 0xFF", m->flash[FO], 0xff);

		/* PAGEBUFCLR discards a pending flash buffer load. */
		m->pc = 0;
		put16(m, 0x00, 0xea0b);			/* LDI r16, 0xAB */
		put16(m, 0x02, 0x9300); put16(m, 0x04, FA);	/* STS 0x8100, r16 */
		step(m); step(m);
		avr_ccp_write(m, AVR_CCP_IOREG);
		cpu_write(m, NV + CTRLA, CMD_PAGEBUFCLR);
		avr_ccp_write(m, AVR_CCP_IOREG);
		cpu_write(m, NV + CTRLA, CMD_PAGEWRITE);
		check("PAGEBUFCLR discarded flash write", m->flash[FO], 0xff);

		/* Section independence: an EEPROM write then a flash write leaves the
		 * shared buffer aimed at flash, so the command acts on flash only. */
		cpu_write(m, EE + 0, 0x55);		/* loads EE buffer (last_section=EE) */
		m->pc = 0;
		put16(m, 0x00, 0xea0b);			/* LDI r16, 0xAB */
		put16(m, 0x02, 0x9300); put16(m, 0x04, FA);	/* STS -> flash (last=FLASH) */
		step(m); step(m);
		avr_ccp_write(m, AVR_CCP_IOREG);
		cpu_write(m, NV + CTRLA, CMD_PAGEWRITE);
		check("command hit flash, not EEPROM", m->flash[FO], 0xab);
		check("EEPROM left uncommitted", cpu_read(m, EE + 0), 0xff);
	}

	printf("== modern RTC counter (sim_tiny3217) ==\n");
	{
		const avr_io_addr_t R = 0x140;
		enum { RTCEN = 0x01 };
		enum { F_OVF = 0x01, F_CMP = 0x02 };
		/* CLK_PER=3.333MHz, RTC src=32.768kHz => ~101 CPU cycles per RTC tick. */

		avr_t *m = avr_make_mcu_by_name("attiny3217");
		if (!m) { printf("cannot make attiny3217 core\n"); return 2; }
		m->log = LOG_ERROR;
		avr_init(m);
		memset(m->flash, 0, 0x2000);	/* NOPs */

		/* PER reset value is 0xFFFF. */
		check("RTC PER resets to 0xFFFF",
			  (m->data[R + RTCR_PERL] | (m->data[R + RTCR_PERH] << 8)), 0xffff);

		cpu_write(m, R + RTCR_PERL, 4);  cpu_write(m, R + RTCR_PERH, 0);
		cpu_write(m, R + RTCR_CMPL, 2);  cpu_write(m, R + RTCR_CMPH, 0);
		cpu_write(m, R + RTCR_INTCTRL, F_CMP);	/* CMP enable, OVF disabled */
		cpu_write(m, R + RTCR_CLKSEL, 0x00);	/* INT32K (32.768 kHz) */
		cpu_write(m, R + RTCR_CTRLA, RTCEN);

		/* CMP match at CNT == 2 => ~202 CPU cycles. */
		long tcmp = -1;
		for (int i = 0; i < 4000 && tcmp < 0; i++) {
			avr_run(m);
			if (m->data[R + RTCR_INTFLAGS] & F_CMP) tcmp = (long)m->cycle;
		}
		check("CMP match near 202 cycles", tcmp >= 196 && tcmp <= 210, 1);
		check("CMP raises (enabled) interrupt", avr_has_pending_interrupts(m), 1);

		/* Live CNT read is in [0, PER]. */
		uint8_t cnt = cpu_read(m, R + RTCR_CNTL);
		check("RTC CNT live read <= PER", cnt <= 4, 1);

		/* W1C clears the CMP flag (the pending FIFO entry only drains on
		 * service, so avr_has_pending stays set here — see below for a clean
		 * masking test on a fresh core). */
		cpu_write(m, R + RTCR_INTFLAGS, F_CMP);
		check("CMP cleared by W1C", !!(m->data[R + RTCR_INTFLAGS] & F_CMP), 0);

		/* OVF fires at the wrap (PER+1 == 5) => ~505 cycles. */
		long tovf = -1;
		for (int i = 0; i < 4000 && tovf < 0; i++) {
			avr_run(m);
			if (m->data[R + RTCR_INTFLAGS] & F_OVF) tovf = (long)m->cycle;
		}
		check("OVF flag set near 505 cycles", tovf >= 495 && tovf <= 515, 1);

		/* Disable: no further events. */
		cpu_write(m, R + RTCR_INTFLAGS, F_OVF | F_CMP);	/* clear both */
		cpu_write(m, R + RTCR_CTRLA, 0x00);
		int fired = 0;
		for (int i = 0; i < 4000; i++) {
			avr_run(m);
			if (m->data[R + RTCR_INTFLAGS] & (F_OVF | F_CMP)) { fired = 1; break; }
		}
		check("no RTC counter event after disable", fired, 0);
	}

	printf("== modern RTC live-reschedule continuity (sim_tiny3217) ==\n");
	{
		const avr_io_addr_t R = 0x140;
		enum { RTCEN = 0x01, F_CMP = 0x02 };
		/* CLK_PER=3.333MHz, RTC src=32.768kHz => ~101 CPU cycles per RTC tick. */

		avr_t *m = avr_make_mcu_by_name("attiny3217");
		if (!m) { printf("cannot make attiny3217 core\n"); return 2; }
		m->log = LOG_ERROR;
		avr_init(m);
		memset(m->flash, 0, 0x2000);	/* NOPs */

		cpu_write(m, R + RTCR_PERL, 0xff); cpu_write(m, R + RTCR_PERH, 0xff);
		cpu_write(m, R + RTCR_CMPL, 0xff); cpu_write(m, R + RTCR_CMPH, 0xff);
		cpu_write(m, R + RTCR_INTCTRL, 0);	/* flags only, no interrupt */
		cpu_write(m, R + RTCR_CLKSEL, 0x00);	/* INT32K */
		cpu_write(m, R + RTCR_CTRLA, RTCEN);

		/* Advance the counter ~10 ticks WITHOUT reading CNT, so the CNT
		 * register stays stale at 0 (it is only refreshed on a CNT read). */
		long s = (long)m->cycle;
		while ((long)m->cycle - s < 1010) avr_run(m);

		/* Move CMP just ahead of the live count. The reschedule must anchor
		 * from the live count (~10 ticks), so the match lands ~2 ticks later.
		 * The pre-fix bug re-anchored from the stale CNT register (0) and
		 * pushed the match out to ~12 ticks (~1200 cycles). */
		cpu_write(m, R + RTCR_CMPL, 12); cpu_write(m, R + RTCR_CMPH, 0);

		long w = (long)m->cycle, tcmp = -1;
		for (int i = 0; i < 4000 && tcmp < 0; i++) {
			avr_run(m);
			if (m->data[R + RTCR_INTFLAGS] & F_CMP) tcmp = (long)m->cycle - w;
		}
		check("CMP match found after live reschedule", tcmp >= 0, 1);
		check("CMP match anchored to live count (not stale CNT)",
			  tcmp >= 0 && tcmp < 600, 1);
	}

	printf("== modern RTC STATUS / PITSTATUS sync-busy (sim_tiny3217) ==\n");
	{
		const avr_io_addr_t R = 0x140;
		enum { RTCEN = 0x01, PITEN = 0x01 };
		enum { CTRLABUSY = 0x01, CNTBUSY = 0x02, PERBUSY = 0x04, CMPBUSY = 0x08 };
		enum { PIT_CTRLBUSY = 0x01 };
		/* CLK_PER=3.333MHz, RTC src=32.768kHz => ~101 CPU cycles per RTC tick;
		 * the 2-cycle sync latency is ~203 CPU cycles (DS40002205A 23.12.2). */

		avr_t *m = avr_make_mcu_by_name("attiny3217");
		if (!m) { printf("cannot make attiny3217 core\n"); return 2; }
		m->log = LOG_ERROR;
		avr_init(m);
		memset(m->flash, 0, 0x2000);	/* NOPs */
		cpu_write(m, R + RTCR_CLKSEL, 0x00);		/* INT32K */

		/* STATUS reads 0 (idle) before any write. */
		check("STATUS idle before writes", cpu_read(m, R + RTCR_STATUS), 0);

		/* A CTRLA write asserts CTRLABUSY for the sync window. */
		cpu_write(m, R + RTCR_CTRLA, RTCEN);
		check("CTRLABUSY set right after CTRLA write",
			  !!(cpu_read(m, R + RTCR_STATUS) & CTRLABUSY), 1);

		long s = (long)m->cycle;
		while ((long)m->cycle - s < 50) avr_run(m);	/* < 2 RTC cycles */
		check("CTRLABUSY still set mid-sync",
			  !!(cpu_read(m, R + RTCR_STATUS) & CTRLABUSY), 1);

		while ((long)m->cycle - s < 260) avr_run(m);	/* past the sync latency */
		check("CTRLABUSY clears after ~2 RTC cycles",
			  !!(cpu_read(m, R + RTCR_STATUS) & CTRLABUSY), 0);

		/* Each synchronised register asserts its own busy bit. */
		cpu_write(m, R + RTCR_CNTL, 0);
		check("CNTBUSY set after CNT write",
			  !!(cpu_read(m, R + RTCR_STATUS) & CNTBUSY), 1);
		cpu_write(m, R + RTCR_PERL, 8);
		check("PERBUSY set after PER write",
			  !!(cpu_read(m, R + RTCR_STATUS) & PERBUSY), 1);
		cpu_write(m, R + RTCR_CMPL, 4);
		check("CMPBUSY set after CMP write",
			  !!(cpu_read(m, R + RTCR_STATUS) & CMPBUSY), 1);

		/* PITCTRLA write asserts PITSTATUS.CTRLBUSY, clearing after the sync. */
		cpu_write(m, R + RTCR_PITCTRLA, PITEN);
		check("PIT CTRLBUSY set after PITCTRLA write",
			  !!(cpu_read(m, R + RTCR_PITSTATUS) & PIT_CTRLBUSY), 1);
		long p0 = (long)m->cycle;
		while ((long)m->cycle - p0 < 260) avr_run(m);
		check("PIT CTRLBUSY clears after ~2 RTC cycles",
			  !!(cpu_read(m, R + RTCR_PITSTATUS) & PIT_CTRLBUSY), 0);
	}

	printf("== modern RTC_CNT interrupt gating (sim_tiny3217) ==\n");
	{
		const avr_io_addr_t R = 0x140;
		enum { RTCEN = 0x01 };
		enum { F_OVF = 0x01, F_CMP = 0x02 };

		/* Fresh core => the pending FIFO starts empty, so avr_has_pending
		 * reflects exactly whether this RTC raised anything. */
		avr_t *m = avr_make_mcu_by_name("attiny3217");
		if (!m) { printf("cannot make attiny3217 core\n"); return 2; }
		m->log = LOG_ERROR;
		avr_init(m);
		memset(m->flash, 0, 0x2000);	/* NOPs */

		cpu_write(m, R + RTCR_PERL, 4);  cpu_write(m, R + RTCR_PERH, 0);
		cpu_write(m, R + RTCR_INTCTRL, 0x00);	/* nothing enabled */
		cpu_write(m, R + RTCR_CLKSEL, 0x00);
		cpu_write(m, R + RTCR_CTRLA, RTCEN);

		/* OVF flag sets, but with OVF masked no interrupt is raised. */
		int seen = 0;
		for (int i = 0; i < 4000 && !seen; i++) {
			avr_run(m);
			if (m->data[R + RTCR_INTFLAGS] & F_OVF) seen = 1;
		}
		check("OVF flag set with OVF masked", seen, 1);
		check("masked OVF raises nothing", avr_has_pending_interrupts(m), 0);

		/* Enabling OVF while its flag is already set raises immediately. */
		cpu_write(m, R + RTCR_INTCTRL, F_OVF);
		check("enabling a set OVF raises now", avr_has_pending_interrupts(m), 1);
	}

	printf("== modern PIT (sim_tiny3217) ==\n");
	{
		const avr_io_addr_t R = 0x140;
		enum { PITEN = 0x01, PERIOD_CYC4 = (1 << 3) };	/* PERIOD field = 1 */
		enum { F_PI = 0x01 };
		/* CYC4 = 4 RTC ticks => 4*101 ~= 406 CPU cycles. */

		avr_t *m = avr_make_mcu_by_name("attiny3217");
		if (!m) { printf("cannot make attiny3217 core\n"); return 2; }
		m->log = LOG_ERROR;
		avr_init(m);
		memset(m->flash, 0, 0x2000);	/* NOPs */

		cpu_write(m, R + RTCR_CLKSEL, 0x00);		/* INT32K */
		cpu_write(m, R + RTCR_PITINTCTRL, F_PI);	/* PI enable */
		cpu_write(m, R + RTCR_PITCTRLA, PITEN | PERIOD_CYC4);

		long t0 = -1;
		for (int i = 0; i < 6000 && t0 < 0; i++) {
			avr_run(m);
			if (m->data[R + RTCR_PITINTFLAGS] & F_PI) t0 = (long)m->cycle;
		}
		check("first PI near 406 cycles", t0 >= 396 && t0 <= 416, 1);
		check("PI raises (enabled) interrupt", avr_has_pending_interrupts(m), 1);

		/* W1C and confirm the periodic cadence (~406 cycles later). */
		cpu_write(m, R + RTCR_PITINTFLAGS, F_PI);
		check("PI cleared by W1C", !!(m->data[R + RTCR_PITINTFLAGS] & F_PI), 0);
		long t1 = -1;
		for (int i = 0; i < 6000 && t1 < 0; i++) {
			avr_run(m);
			if (m->data[R + RTCR_PITINTFLAGS] & F_PI) t1 = (long)m->cycle;
		}
		check("second PI ~406 cycles later",
			  (t1 - t0) >= 396 && (t1 - t0) <= 416, 1);

		/* Disable PIT: no further events. */
		cpu_write(m, R + RTCR_PITINTFLAGS, F_PI);
		cpu_write(m, R + RTCR_PITCTRLA, 0x00);
		int fired = 0;
		for (int i = 0; i < 6000; i++) {
			avr_run(m);
			if (m->data[R + RTCR_PITINTFLAGS] & F_PI) { fired = 1; break; }
		}
		check("no PI after disable", fired, 0);
	}

	printf("== modern ADC0 (sim_tiny3217) ==\n");
	{
		const avr_io_addr_t A = 0x600;
		enum { ENABLE = 0x01, FREERUN = 0x02, RESSEL_8BIT = 0x04 };
		enum { STCONV = 0x01 };
		enum { F_RESRDY = 0x01, F_WCMP = 0x02 };

		avr_t *m = avr_make_mcu_by_name("attiny3217");
		if (!m) { printf("cannot make attiny3217 core\n"); return 2; }
		m->log = LOG_ERROR;
		avr_init(m);
		memset(m->flash, 0, 0x2000);	/* NOPs */

		/* Present 1650 mV on AIN3 (half of the default 3300 mV vref). */
		avr_irq_t *ain3 = avr_io_getirq(m, AVR_IOCTL_ADCM_GETIRQ('0'), 3);
		check("ADC AIN3 irq exists", ain3 != NULL, 1);
		avr_raise_irq(ain3, 1650);

		/* 10-bit single shot: 1650/3300 * 1024 = 512. */
		cpu_write(m, A + ADCMR_MUXPOS, 3);
		cpu_write(m, A + ADCMR_INTCTRL, F_RESRDY);
		cpu_write(m, A + ADCMR_CTRLA, ENABLE);		/* 10-bit, PRESC DIV2 */
		long t0 = (long)m->cycle;
		cpu_write(m, A + ADCMR_COMMAND, STCONV);

		long tr = -1;
		for (int i = 0; i < 400 && tr < 0; i++) {
			avr_run(m);
			if (m->data[A + ADCMR_INTFLAGS] & F_RESRDY) tr = (long)m->cycle;
		}
		check("RESRDY after ~26 cycles (DIV2)", (tr - t0) >= 24 && (tr - t0) <= 32, 1);
		uint16_t res = m->data[A + ADCMR_RESL] | (m->data[A + ADCMR_RESH] << 8);
		check("10-bit result = 512", res, 512);
		check("RESRDY raises (enabled) interrupt", avr_has_pending_interrupts(m), 1);
		check("STCONV self-cleared", !!(m->data[A + ADCMR_COMMAND] & STCONV), 0);

		/* W1C the flag. */
		cpu_write(m, A + ADCMR_INTFLAGS, F_RESRDY);
		check("RESRDY cleared by W1C", !!(m->data[A + ADCMR_INTFLAGS] & F_RESRDY), 0);

		/* 8-bit single shot: 1650/3300 * 256 = 128. */
		cpu_write(m, A + ADCMR_CTRLA, ENABLE | RESSEL_8BIT);
		cpu_write(m, A + ADCMR_COMMAND, STCONV);
		for (int i = 0; i < 400 && !(m->data[A + ADCMR_INTFLAGS] & F_RESRDY); i++)
			avr_run(m);
		res = m->data[A + ADCMR_RESL] | (m->data[A + ADCMR_RESH] << 8);
		check("8-bit result = 128", res, 128);
		cpu_write(m, A + ADCMR_INTFLAGS, F_RESRDY);

		/* Free-running mode: conversions keep coming (~26 cycles apart). */
		cpu_write(m, A + ADCMR_CTRLA, ENABLE | FREERUN);	/* back to 10-bit */
		int hits = 0;
		long last = -1, maxgap = 0;
		for (int i = 0; i < 4000 && hits < 4; i++) {
			avr_run(m);
			if (m->data[A + ADCMR_INTFLAGS] & F_RESRDY) {
				if (last >= 0 && (long)m->cycle - last > maxgap)
					maxgap = (long)m->cycle - last;
				last = (long)m->cycle;
				hits++;
				cpu_write(m, A + ADCMR_INTFLAGS, F_RESRDY);	/* W1C, expect re-set */
			}
		}
		check("free-run produces repeated conversions", hits >= 4, 1);
		check("free-run cadence ~26 cycles", maxgap >= 24 && maxgap <= 34, 1);

		/* Stop free-running; the stream halts. */
		cpu_write(m, A + ADCMR_CTRLA, 0x00);		/* disable */
		cpu_write(m, A + ADCMR_INTFLAGS, F_RESRDY);
		int fired = 0;
		for (int i = 0; i < 400; i++) {
			avr_run(m);
			if (m->data[A + ADCMR_INTFLAGS] & F_RESRDY) { fired = 1; break; }
		}
		check("no conversion after disable", fired, 0);
	}

	printf("== modern ADC0 window comparator (sim_tiny3217) ==\n");
	{
		const avr_io_addr_t A = 0x600;
		enum { ENABLE = 0x01, STCONV = 0x01 };
		enum { F_RESRDY = 0x01, F_WCMP = 0x02 };
		enum { WINCM_ABOVE = 2, WINCM_INSIDE = 3 };

		avr_t *m = avr_make_mcu_by_name("attiny3217");
		if (!m) { printf("cannot make attiny3217 core\n"); return 2; }
		m->log = LOG_ERROR;
		avr_init(m);
		memset(m->flash, 0, 0x2000);

		avr_irq_t *ain0 = avr_io_getirq(m, AVR_IOCTL_ADCM_GETIRQ('0'), 0);
		avr_raise_irq(ain0, 1650);	/* => result 512 (10-bit) */

		/* ABOVE window with WINHT=100: result 512 > 100 => WCMP fires. */
		cpu_write(m, A + ADCMR_MUXPOS, 0);
		cpu_write(m, A + ADCMR_WINHTL, 100); cpu_write(m, A + ADCMR_WINHTL + 1, 0);
		cpu_write(m, A + ADCMR_CTRLE, WINCM_ABOVE);
		cpu_write(m, A + ADCMR_INTCTRL, F_WCMP);	/* WCMP enable only */
		cpu_write(m, A + ADCMR_CTRLA, ENABLE);
		cpu_write(m, A + ADCMR_COMMAND, STCONV);
		for (int i = 0; i < 400 && !(m->data[A + ADCMR_INTFLAGS] & F_RESRDY); i++)
			avr_run(m);
		check("WCMP set (512 ABOVE 100)", !!(m->data[A + ADCMR_INTFLAGS] & F_WCMP), 1);
		check("WCMP raises (enabled) interrupt", avr_has_pending_interrupts(m), 1);

		/* INSIDE [600,700]: result 512 is outside => WCMP must NOT fire. */
		cpu_write(m, A + ADCMR_INTFLAGS, F_WCMP | F_RESRDY);
		cpu_write(m, A + ADCMR_WINLTL, 600 & 0xff);
		cpu_write(m, A + ADCMR_WINLTL + 1, 600 >> 8);
		cpu_write(m, A + ADCMR_WINHTL, 700 & 0xff);
		cpu_write(m, A + ADCMR_WINHTL + 1, 700 >> 8);
		cpu_write(m, A + ADCMR_CTRLE, WINCM_INSIDE);
		cpu_write(m, A + ADCMR_COMMAND, STCONV);
		for (int i = 0; i < 400 && !(m->data[A + ADCMR_INTFLAGS] & F_RESRDY); i++)
			avr_run(m);
		check("WCMP not set (512 outside [600,700])",
			  !!(m->data[A + ADCMR_INTFLAGS] & F_WCMP), 0);
	}

	printf("== modern ADC0 accumulation + internal channels (sim_tiny3217) ==\n");
	{
		const avr_io_addr_t A = 0x600;
		enum { ENABLE = 0x01, STCONV = 0x01 };
		enum { F_RESRDY = 0x01 };
		enum { SAMPNUM_ACC4 = 2 };	/* CTRLB value 2 => 4 samples */

		avr_t *m = avr_make_mcu_by_name("attiny3217");
		if (!m) { printf("cannot make attiny3217 core\n"); return 2; }
		m->log = LOG_ERROR;
		avr_init(m);
		memset(m->flash, 0, 0x2000);

		#define ADC_CONVERT() do { \
			cpu_write(m, A + ADCMR_INTFLAGS, F_RESRDY); \
			cpu_write(m, A + ADCMR_COMMAND, STCONV); \
			for (int i = 0; i < 4000 && \
				 !(m->data[A + ADCMR_INTFLAGS] & F_RESRDY); i++) avr_run(m); \
		} while (0)
		#define ADC_RES() (cpu_read(m, A + ADCMR_RESL) | \
						   (cpu_read(m, A + ADCMR_RESH) << 8))

		/* AIN0 = 1650 mV, vref 3300 => single 10-bit sample = 512. */
		avr_raise_irq(avr_io_getirq(m, AVR_IOCTL_ADCM_GETIRQ('0'), 0), 1650);
		cpu_write(m, A + ADCMR_MUXPOS, 0);
		cpu_write(m, A + ADCMR_CTRLA, ENABLE);

		/* SAMPNUM=0: a single sample. */
		ADC_CONVERT();
		check("single sample = 512", ADC_RES(), 512);

		/* SAMPNUM=ACC4: the result is the sum of 4 samples = 2048. */
		cpu_write(m, A + ADCMR_CTRLB, SAMPNUM_ACC4);
		ADC_CONVERT();
		check("4-sample accumulation = 2048", ADC_RES(), 2048);
		cpu_write(m, A + ADCMR_CTRLB, 0);	/* back to single */

		/* GND internal channel (0x1F) reads 0. */
		cpu_write(m, A + ADCMR_MUXPOS, AVR_ADCM_CH_GND);
		ADC_CONVERT();
		check("GND channel reads 0", ADC_RES(), 0);

		/* DAC0 internal channel (0x1C): DAC0 output is wired to it. Enable DAC0
		 * (vref 1100) with DATA=128 => 550 mV; ADC (vref 3300) => 550*1024/3300
		 * = 170. */
		cpu_write(m, 0x6a0 + 0x00, 0x01);	/* DAC0.CTRLA = ENABLE */
		cpu_write(m, 0x6a0 + 0x01, 128);	/* DAC0.DATA */
		cpu_write(m, A + ADCMR_MUXPOS, AVR_ADCM_CH_DAC0);
		ADC_CONVERT();
		check("DAC0 channel reads DAC output (~170)", ADC_RES(), 170);

		#undef ADC_CONVERT
		#undef ADC_RES
	}

	printf("== modern ADC0 wide coverage (sim_tiny3217) ==\n");
	{
		const avr_io_addr_t A = 0x600;
		enum { ENABLE = 0x01, FREERUN = 0x02, RESSEL8 = 0x04, STCONV = 0x01 };
		enum { F_RESRDY = 0x01, F_WCMP = 0x02 };
		enum { WINCM_BELOW = 1, WINCM_ABOVE = 2, WINCM_INSIDE = 3,
			   WINCM_OUTSIDE = 4, WINCM_NONE = 0 };

		avr_t *m = avr_make_mcu_by_name("attiny3217");
		if (!m) { printf("cannot make attiny3217 core\n"); return 2; }
		m->log = LOG_ERROR;
		avr_init(m);
		memset(m->flash, 0, 0x2000);

		#define ADC_CONVERT() do { \
			cpu_write(m, A + ADCMR_INTFLAGS, F_RESRDY | F_WCMP);	/* W1C clear */ \
			cpu_write(m, A + ADCMR_COMMAND, STCONV); \
			for (int i = 0; i < 4000 && \
				 !(m->data[A + ADCMR_INTFLAGS] & F_RESRDY); i++) avr_run(m); \
		} while (0)
		#define ADC_RES() (cpu_read(m, A + ADCMR_RESL) | \
						   (cpu_read(m, A + ADCMR_RESH) << 8))
		#define SET_MV(ch,mv) avr_raise_irq( \
			avr_io_getirq(m, AVR_IOCTL_ADCM_GETIRQ('0'), (ch)), (mv))

		cpu_write(m, A + ADCMR_MUXPOS, 3);
		SET_MV(3, 1650);

		/* 8-bit resolution (CTRLA.RESSEL): 1650/3300 -> 128, and clamp at 255. */
		cpu_write(m, A + ADCMR_CTRLA, ENABLE | RESSEL8);
		ADC_CONVERT();  check("8-bit 1650mV -> 128", ADC_RES(), 128);
		SET_MV(3, 3300); ADC_CONVERT(); check("8-bit clamps to 255", ADC_RES(), 255);

		/* 10-bit clamp when input >= vref. */
		cpu_write(m, A + ADCMR_CTRLA, ENABLE);
		SET_MV(3, 5000); ADC_CONVERT(); check("10-bit >vref clamps to 1023", ADC_RES(), 1023);
		SET_MV(3, 1650);

		/* All window-comparator modes against a 512 result, window [100,200]. */
		cpu_write(m, A + ADCMR_WINLTL, 100); cpu_write(m, A + ADCMR_WINLTL + 1, 0);
		cpu_write(m, A + ADCMR_WINHTL, 200); cpu_write(m, A + ADCMR_WINHTL + 1, 0);
		cpu_write(m, A + ADCMR_CTRLE, WINCM_BELOW);
		ADC_CONVERT(); check("WIN BELOW: 512<100 -> no", !!(m->data[A+ADCMR_INTFLAGS]&F_WCMP), 0);
		cpu_write(m, A + ADCMR_CTRLE, WINCM_ABOVE);
		ADC_CONVERT(); check("WIN ABOVE: 512>200 -> yes", !!(m->data[A+ADCMR_INTFLAGS]&F_WCMP), 1);
		cpu_write(m, A + ADCMR_CTRLE, WINCM_INSIDE);
		ADC_CONVERT(); check("WIN INSIDE [100,200]: 512 -> no", !!(m->data[A+ADCMR_INTFLAGS]&F_WCMP), 0);
		cpu_write(m, A + ADCMR_CTRLE, WINCM_OUTSIDE);
		ADC_CONVERT(); check("WIN OUTSIDE [100,200]: 512 -> yes", !!(m->data[A+ADCMR_INTFLAGS]&F_WCMP), 1);
		cpu_write(m, A + ADCMR_CTRLE, WINCM_NONE);
		ADC_CONVERT(); check("WIN NONE: never flags", !!(m->data[A+ADCMR_INTFLAGS]&F_WCMP), 0);
		cpu_write(m, A + ADCMR_CTRLE, 0);

		/* 64-sample accumulation sum (no averaging): 64 * 512 = 32768. */
		cpu_write(m, A + ADCMR_CTRLB, 6);
		ADC_CONVERT(); check("64-sample accumulation = 32768", ADC_RES(), 32768);
		cpu_write(m, A + ADCMR_CTRLB, 0);

		/* STCONV self-clears on completion. */
		cpu_write(m, A + ADCMR_INTFLAGS, F_RESRDY);
		cpu_write(m, A + ADCMR_COMMAND, STCONV);
		check("STCONV busy immediately after start",
			  !!(m->data[A + ADCMR_COMMAND] & STCONV), 1);
		for (int i = 0; i < 4000 && !(m->data[A+ADCMR_INTFLAGS]&F_RESRDY); i++) avr_run(m);
		check("STCONV self-clears on completion",
			  !!(m->data[A + ADCMR_COMMAND] & STCONV), 0);

		/* Disabling the ADC cancels an in-flight conversion (no RESRDY). */
		cpu_write(m, A + ADCMR_INTFLAGS, F_RESRDY);
		cpu_write(m, A + ADCMR_COMMAND, STCONV);
		cpu_write(m, A + ADCMR_CTRLA, 0);	/* disable mid-flight */
		for (int i = 0; i < 2000; i++) avr_run(m);
		check("disable cancels in-flight conversion",
			  !!(m->data[A + ADCMR_INTFLAGS] & F_RESRDY), 0);

		/* Free-running keeps converting without further STCONV writes. */
		cpu_write(m, A + ADCMR_CTRLA, ENABLE | FREERUN);
		for (int i = 0; i < 2000; i++) avr_run(m);
		check("free-run produced a result", ADC_RES(), 512);
		cpu_write(m, A + ADCMR_INTFLAGS, F_RESRDY);	/* W1C clear */
		for (int i = 0; i < 2000; i++) avr_run(m);
		check("free-run re-sets RESRDY", !!(m->data[A+ADCMR_INTFLAGS]&F_RESRDY), 1);
		cpu_write(m, A + ADCMR_CTRLA, ENABLE);

		/* Temperature-sensor channel: code recovers ~300 K via the datasheet
		 * transfer function with the modelled SIGROW cal. */
		SET_MV(AVR_ADCM_CH_TEMPSENSE, 300);	/* die temp in Kelvin on the temp IRQ */
		cpu_write(m, A + ADCMR_MUXPOS, AVR_ADCM_CH_TEMPSENSE);
		ADC_CONVERT();
		{
			int code = ADC_RES();
			int off = (int8_t)cpu_read(m, 0x1100 + 0x21);
			int gain = cpu_read(m, 0x1100 + 0x20);
			int tk = ((code - off) * gain + 0x80) >> 8;
			check("temp-sensor round-trip ~300K", (tk >= 298 && tk <= 302), 1);
		}

		#undef ADC_CONVERT
		#undef ADC_RES
		#undef SET_MV
	}

	printf("== modern ADC0 reference select (CTRLC.REFSEL + VREF) (sim_tiny3217) ==\n");
	{
		const avr_io_addr_t A = 0x600, V = 0x0a0;
		enum { ENABLE = 0x01, STCONV = 0x01, CTRLC = 0x02 };
		enum { F_RESRDY = 0x01 };
		enum { REFSEL_VDD = (1 << 4) };		/* CTRLC.REFSEL = VDD */

		avr_t *m = avr_make_mcu_by_name("attiny3217");
		if (!m) { printf("cannot make attiny3217 core\n"); return 2; }
		m->log = LOG_ERROR;
		avr_init(m);
		memset(m->flash, 0, 0x2000);

		#define ADC_CONVERT() do { \
			cpu_write(m, A + ADCMR_INTFLAGS, F_RESRDY); \
			cpu_write(m, A + ADCMR_COMMAND, STCONV); \
			for (int i = 0; i < 4000 && \
				 !(m->data[A + ADCMR_INTFLAGS] & F_RESRDY); i++) avr_run(m); \
		} while (0)
		#define ADC_RES() (cpu_read(m, A + ADCMR_RESL) | \
						   (cpu_read(m, A + ADCMR_RESH) << 8))

		avr_raise_irq(avr_io_getirq(m, AVR_IOCTL_ADCM_GETIRQ('0'), 0), 550);
		cpu_write(m, A + ADCMR_MUXPOS, 0);
		cpu_write(m, A + ADCMR_CTRLA, ENABLE);

		/* Default REFSEL=INTREF, internal ref defaults to 3300 mV: 550 -> 170. */
		ADC_CONVERT();
		check("INTREF defaults to 3300 mV", ADC_RES(), 170);

		/* Program VREF.ADC0REFSEL=1 (1.1 V): the ADC internal ref follows it, so
		 * 550 mV -> 550*1024/1100 = 512. */
		cpu_write(m, V + 0x00, (1 << 4));	/* VREF.CTRLA ADC0REFSEL = 1.1V */
		ADC_CONVERT();
		check("INTREF tracks VREF (1.1V) => 512", ADC_RES(), 512);

		/* Switching REFSEL to VDD ignores VREF and uses the 3300 mV supply ref. */
		cpu_write(m, A + CTRLC, REFSEL_VDD);
		ADC_CONVERT();
		check("REFSEL=VDD uses 3300 mV => 170", ADC_RES(), 170);

		#undef ADC_CONVERT
		#undef ADC_RES
	}

	printf("== modern ADC0 temperature sensor (sim_tiny3217) ==\n");
	{
		const avr_io_addr_t A = 0x600, SIG = 0x1100;
		enum { ENABLE = 0x01, STCONV = 0x01 };
		enum { F_RESRDY = 0x01 };

		avr_t *m = avr_make_mcu_by_name("attiny3217");
		if (!m) { printf("cannot make attiny3217 core\n"); return 2; }
		m->log = LOG_ERROR;
		avr_init(m);
		memset(m->flash, 0, 0x2000);

		/* SIGROW carries the temp-sensor calibration. */
		uint8_t gain = cpu_read(m, SIG + SIGROWR_TEMPSENSE0);
		int8_t  off  = (int8_t)cpu_read(m, SIG + SIGROWR_TEMPSENSE1);
		check("SIGROW.TEMPSENSE0 (gain) populated", gain, 128);
		check("SIGROW.TEMPSENSE1 (offset) populated", off, 50);

		cpu_write(m, A + ADCMR_MUXPOS, AVR_ADCM_CH_TEMPSENSE);
		cpu_write(m, A + ADCMR_CTRLA, ENABLE);

		#define ADC_CONVERT() do { \
			cpu_write(m, A + ADCMR_INTFLAGS, F_RESRDY); \
			cpu_write(m, A + ADCMR_COMMAND, STCONV); \
			for (int i = 0; i < 4000 && \
				 !(m->data[A + ADCMR_INTFLAGS] & F_RESRDY); i++) avr_run(m); \
		} while (0)
		#define ADC_RES() (cpu_read(m, A + ADCMR_RESL) | \
						   (cpu_read(m, A + ADCMR_RESH) << 8))
		/* Datasheet transfer function applied by firmware. */
		#define TEMP_K(res) ((((int32_t)(res) - off) * gain + 0x80) >> 8)

		/* Set the die temperature to 350 K via the temp-sensor channel IRQ. */
		avr_raise_irq(avr_io_getirq(m, AVR_IOCTL_ADCM_GETIRQ('0'),
									AVR_ADCM_CH_TEMPSENSE), 350);
		ADC_CONVERT();
		check("temp channel round-trips 350 K", TEMP_K(ADC_RES()), 350);

		/* And a second temperature. */
		avr_raise_irq(avr_io_getirq(m, AVR_IOCTL_ADCM_GETIRQ('0'),
									AVR_ADCM_CH_TEMPSENSE), 298);
		ADC_CONVERT();
		check("temp channel round-trips 298 K", TEMP_K(ADC_RES()), 298);

		#undef ADC_CONVERT
		#undef ADC_RES
		#undef TEMP_K
	}

	printf("== modern SPI0 host (sim_tiny3217) ==\n");
	{
		const avr_io_addr_t S = 0x820;
		enum { ENABLE = 0x01, MASTER = 0x20 };	/* PRESC DIV4 (8*4=32 cyc) */
		enum { IE = 0x01 };
		enum { F_WRCOL = 0x40, F_IF = 0x80 };

		avr_t *m = avr_make_mcu_by_name("attiny3217");
		if (!m) { printf("cannot make attiny3217 core\n"); return 2; }
		m->log = LOG_ERROR;
		avr_init(m);
		memset(m->flash, 0, 0x2000);	/* NOPs */

		/* Wire an echo "client" onto MOSI that replies with the complement. */
		g_spi_avr = m;
		avr_irq_register_notify(
			avr_io_getirq(m, AVR_IOCTL_SPI_GETIRQ('0'), SPI_IRQ_OUTPUT),
			spi_echo_hook, NULL);

		cpu_write(m, S + SPIMR_CTRLA, ENABLE | MASTER);
		cpu_write(m, S + SPIMR_INTCTRL, IE);

		long t0 = (long)m->cycle;
		cpu_write(m, S + SPIMR_DATA, 0x5a);
		check("IF not set immediately", !!(m->data[S + SPIMR_INTFLAGS] & F_IF), 0);

		long tif = -1;
		for (int i = 0; i < 400 && tif < 0; i++) {
			avr_run(m);
			if (m->data[S + SPIMR_INTFLAGS] & F_IF) tif = (long)m->cycle;
		}
		check("IF set after ~32 cycles (DIV4)", (tif - t0) >= 28 && (tif - t0) <= 40, 1);
		check("MOSI byte seen by client", g_spi_mosi, 0x5a);
		check("received MISO (complement)", cpu_read(m, S + SPIMR_DATA), 0xa5);
		check("transfer raises (IE) interrupt", avr_has_pending_interrupts(m), 1);

		/* Reading DATA cleared IF. */
		check("IF cleared by DATA read", !!(m->data[S + SPIMR_INTFLAGS] & F_IF), 0);

		/* Write collision: a second DATA write before the transfer finishes. */
		cpu_write(m, S + SPIMR_DATA, 0x11);	/* starts a transfer (busy) */
		cpu_write(m, S + SPIMR_DATA, 0x22);	/* mid-flight => WRCOL, ignored */
		check("WRCOL set on mid-transfer write",
			  !!(m->data[S + SPIMR_INTFLAGS] & F_WRCOL), 1);
		for (int i = 0; i < 400 && !(m->data[S + SPIMR_INTFLAGS] & F_IF); i++)
			avr_run(m);
		check("first byte (0x11) clocked, not 0x22", g_spi_mosi, 0x11);
	}

	printf("== modern SPI0 BUFEN mode-switch teardown (sim_tiny3217) ==\n");
	{
		const avr_io_addr_t S = 0x820;
		enum { ENABLE = 0x01, MASTER = 0x20, BUFEN = 0x80 };

		avr_t *m = avr_make_mcu_by_name("attiny3217");
		if (!m) { printf("cannot make attiny3217 core\n"); return 2; }
		m->log = LOG_ERROR;
		avr_init(m);
		memset(m->flash, 0, 0x2000);	/* NOPs */

		g_spi_avr = m;
		g_spi_mosi = 0;
		avr_irq_register_notify(
			avr_io_getirq(m, AVR_IOCTL_SPI_GETIRQ('0'), SPI_IRQ_OUTPUT),
			spi_echo_hook, NULL);

		/* Buffered master: kick off a transfer, then flip BUFEN mid-flight. */
		cpu_write(m, S + SPIMR_CTRLA, ENABLE | MASTER);
		cpu_write(m, S + SPIMR_CTRLB, BUFEN);
		cpu_write(m, S + SPIMR_DATA, 0x5a);	/* schedules a transfer (busy) */
		cpu_write(m, S + SPIMR_CTRLB, 0);	/* switch to unbuffered mid-flight */

		/* The in-flight transfer must be torn down: it may not complete later
		 * under the new mode and leak a stale MOSI byte, interrupt or flag. */
		for (int i = 0; i < 200; i++) avr_run(m);
		check("cancelled transfer emits no MOSI byte", g_spi_mosi, 0);
		check("no stale interrupt after BUFEN switch",
			  avr_has_pending_interrupts(m), 0);
		check("INTFLAGS clear after BUFEN switch",
			  m->data[S + SPIMR_INTFLAGS], 0);
	}

	printf("== modern SPI0 client (sim_tiny3217) ==\n");
	{
		const avr_io_addr_t S = 0x820;
		enum { ENABLE = 0x01 };	/* MASTER clear => client */
		enum { F_IF = 0x80 };

		avr_t *m = avr_make_mcu_by_name("attiny3217");
		if (!m) { printf("cannot make attiny3217 core\n"); return 2; }
		m->log = LOG_ERROR;
		avr_init(m);
		memset(m->flash, 0, 0x2000);

		g_spi_out = 0;
		avr_irq_register_notify(
			avr_io_getirq(m, AVR_IOCTL_SPI_GETIRQ('0'), SPI_IRQ_OUTPUT),
			spi_capture_hook, NULL);

		cpu_write(m, S + SPIMR_CTRLA, ENABLE);	/* client */
		cpu_write(m, S + SPIMR_DATA, 0x3c);	/* byte to shift out on next clock */

		/* A host clocks 0x77 into us: we latch it and echo our 0x3c back. */
		avr_irq_t *in = avr_io_getirq(m, AVR_IOCTL_SPI_GETIRQ('0'), SPI_IRQ_INPUT);
		avr_raise_irq(in, 0x77);
		/* Check IF before reading DATA (a DATA read clears IF). */
		check("client IF set on receive", !!(m->data[S + SPIMR_INTFLAGS] & F_IF), 1);
		check("client latched received byte", m->data[S + SPIMR_DATA], 0x77);
		check("client echoed its DATA on MISO", g_spi_out, 0x3c);
		check("DATA read clears client IF",
			  (cpu_read(m, S + SPIMR_DATA), !!(m->data[S + SPIMR_INTFLAGS] & F_IF)), 0);
	}

	printf("== modern AC0 (sim_tiny3217) ==\n");
	{
		const avr_io_addr_t C = 0x680;
		enum { ENABLE = 0x01, INTMODE_POSEDGE = 0x30 };
		enum { INVERT = 0x80, MUXNEG_VREF = 0x02 };
		enum { CMP = 0x01, STATE = 0x10 };

		avr_t *m = avr_make_mcu_by_name("attiny3217");
		if (!m) { printf("cannot make attiny3217 core\n"); return 2; }
		m->log = LOG_ERROR;
		avr_init(m);
		memset(m->flash, 0, 0x2000);

		avr_irq_t *ainp0 = avr_io_getirq(m, AVR_IOCTL_AC_GETIRQ('0'), AVR_AC_IRQ_AINP0);
		avr_irq_t *ainn0 = avr_io_getirq(m, AVR_IOCTL_AC_GETIRQ('0'), AVR_AC_IRQ_AINN0);
		check("AC AINP0 irq exists", ainp0 != NULL, 1);
		uint8_t out = 0xff;
		avr_irq_register_notify(
			avr_io_getirq(m, AVR_IOCTL_AC_GETIRQ('0'), AVR_AC_IRQ_OUT), rec_irq, &out);

		/* V+ = 2000 mV, V- = 1000 mV (AINN0), MUXPOS/NEG = PIN0. */
		avr_raise_irq(ainp0, 2000);
		avr_raise_irq(ainn0, 1000);
		check("STATE 0 while disabled", !!(cpu_read(m, C + ACR_STATUS) & STATE), 0);

		cpu_write(m, C + ACR_CTRLA, ENABLE);	/* INTMODE BOTHEDGE, no int */
		check("STATE 1 (V+ > V-)", !!(cpu_read(m, C + ACR_STATUS) & STATE), 1);
		check("OUT IRQ went high", out, 1);

		/* Drop V+ below V-: output falls. */
		avr_raise_irq(ainp0, 500);
		check("STATE 0 (V+ < V-)", !!(cpu_read(m, C + ACR_STATUS) & STATE), 0);
		check("OUT IRQ went low", out, 0);

		/* INVERT flips the output for the same inputs. */
		cpu_write(m, C + ACR_MUXCTRLA, INVERT);	/* MUXPOS/NEG PIN0, inverted */
		check("INVERT => STATE 1 (V+ < V-)", !!(cpu_read(m, C + ACR_STATUS) & STATE), 1);
		cpu_write(m, C + ACR_MUXCTRLA, 0x00);	/* back to normal */

		/* Positive-edge interrupt. */
		cpu_write(m, C + ACR_STATUS, CMP);		/* clear any stale flag */
		cpu_write(m, C + ACR_CTRLA, ENABLE | INTMODE_POSEDGE);
		cpu_write(m, C + ACR_INTCTRL, CMP);		/* enable AC interrupt */
		avr_raise_irq(ainp0, 500);			/* ensure low first */
		cpu_write(m, C + ACR_STATUS, CMP);		/* clear */

		avr_raise_irq(ainp0, 2000);			/* low->high: posedge */
		check("posedge sets CMP flag", !!(m->data[C + ACR_STATUS] & CMP), 1);
		check("AC raises (enabled) interrupt", avr_has_pending_interrupts(m), 1);

		/* W1C the flag; a negative edge must NOT set it in POSEDGE mode. */
		cpu_write(m, C + ACR_STATUS, CMP);
		check("CMP cleared by W1C", !!(m->data[C + ACR_STATUS] & CMP), 0);
		avr_raise_irq(ainp0, 300);			/* high->low: negedge */
		check("negedge ignored in POSEDGE mode", !!(m->data[C + ACR_STATUS] & CMP), 0);

		/* MUXNEG = VREF (default 1100 mV): compare against the reference. */
		cpu_write(m, C + ACR_STATUS, CMP);
		cpu_write(m, C + ACR_MUXCTRLA, MUXNEG_VREF);
		avr_raise_irq(ainp0, 2000);			/* 2000 > 1100 */
		check("STATE 1 vs VREF (2000 > 1100)", !!(cpu_read(m, C + ACR_STATUS) & STATE), 1);
		avr_raise_irq(ainp0, 800);			/* 800 < 1100 */
		check("STATE 0 vs VREF (800 < 1100)", !!(cpu_read(m, C + ACR_STATUS) & STATE), 0);

		/* Input hysteresis (CTRLA.HYSMODE = 0x2 => ±25 mV). With V- = 1000 mV,
		 * the output holds until V+ crosses by more than the band
		 * (DS40002205A 29.3.2.1 / 29.5.1). */
		enum { HYSMODE_25 = 0x04 };
		cpu_write(m, C + ACR_INTCTRL, 0x00);		/* no interrupt for this part */
		cpu_write(m, C + ACR_MUXCTRLA, 0x00);		/* PIN0/PIN0, not inverted */
		avr_raise_irq(ainn0, 1000);
		avr_raise_irq(ainp0, 900);			/* start clearly low */
		cpu_write(m, C + ACR_CTRLA, ENABLE | HYSMODE_25);
		check("hyst: STATE 0 at V+=900", !!(cpu_read(m, C + ACR_STATUS) & STATE), 0);

		avr_raise_irq(ainp0, 1010);			/* +10 mV: inside band */
		check("hyst: holds low at V+=1010 (<+25)", !!(cpu_read(m, C + ACR_STATUS) & STATE), 0);
		avr_raise_irq(ainp0, 1030);			/* +30 mV: crosses +band */
		check("hyst: flips high at V+=1030 (>+25)", !!(cpu_read(m, C + ACR_STATUS) & STATE), 1);
		avr_raise_irq(ainp0, 990);			/* -10 mV: inside band */
		check("hyst: holds high at V+=990 (>-25)", !!(cpu_read(m, C + ACR_STATUS) & STATE), 1);
		avr_raise_irq(ainp0, 970);			/* -30 mV: crosses -band */
		check("hyst: flips low at V+=970 (<-25)", !!(cpu_read(m, C + ACR_STATUS) & STATE), 0);

		/* HYSMODE OFF: the same +10 mV crossing flips immediately. */
		cpu_write(m, C + ACR_CTRLA, ENABLE);		/* HYSMODE = OFF */
		avr_raise_irq(ainp0, 1010);
		check("no hyst: flips high at V+=1010", !!(cpu_read(m, C + ACR_STATUS) & STATE), 1);
	}

	printf("== modern DAC0 (sim_tiny3217) ==\n");
	{
		const avr_io_addr_t D = 0x6a0;
		enum { ENABLE = 0x01, OUTEN = 0x40 };
		/* default vref = 1100 mV; out = DATA * 1100 / 256. */

		avr_t *m = avr_make_mcu_by_name("attiny3217");
		if (!m) { printf("cannot make attiny3217 core\n"); return 2; }
		m->log = LOG_ERROR;
		avr_init(m);
		memset(m->flash, 0, 0x2000);

		g_dac_out = 0xffffffff;
		g_dac_pin = 0xffffffff;
		avr_irq_register_notify(
			avr_io_getirq(m, AVR_IOCTL_DAC_GETIRQ('0'), AVR_DAC_IRQ_OUT),
			dac_capture_hook, NULL);
		avr_irq_register_notify(
			avr_io_getirq(m, AVR_IOCTL_DAC_GETIRQ('0'), AVR_DAC_IRQ_PIN),
			dac_pin_capture_hook, NULL);

		/* DATA written while disabled produces no output. */
		cpu_write(m, D + DACR_DATA, 128);
		check("no DAC output while disabled", g_dac_out, 0xffffffff);

		/* Enable: 128 * 1100 / 256 = 550 mV. */
		cpu_write(m, D + DACR_CTRLA, ENABLE);
		check("DAC out 550 mV (DATA=128)", g_dac_out, 550);

		/* Full-scale: 255 * 1100 / 256 = 1095 mV. */
		cpu_write(m, D + DACR_DATA, 255);
		check("DAC out 1095 mV (DATA=255)", g_dac_out, 1095);

		/* Zero. */
		cpu_write(m, D + DACR_DATA, 0);
		check("DAC out 0 mV (DATA=0)", g_dac_out, 0);

		/* Disable forces the output to 0 even with DATA set. */
		cpu_write(m, D + DACR_DATA, 200);
		cpu_write(m, D + DACR_CTRLA, 0x00);
		check("DAC out 0 after disable", g_dac_out, 0);

		/* Output buffer (OUTEN): the internal OUT signal is available whenever
		 * ENABLE=1, but the pin is driven only when OUTEN=1 as well
		 * (DS40002205A 31.3.2.3). */
		g_dac_pin = 0xffffffff;
		cpu_write(m, D + DACR_DATA, 128);
		cpu_write(m, D + DACR_CTRLA, ENABLE);		/* ENABLE, OUTEN=0 */
		check("internal OUT driven without OUTEN", g_dac_out, 550);
		check("pin NOT driven while OUTEN=0", g_dac_pin, 0xffffffff);

		cpu_write(m, D + DACR_CTRLA, ENABLE | OUTEN);	/* enable output buffer */
		check("pin driven once OUTEN=1", g_dac_pin, 550);

		cpu_write(m, D + DACR_DATA, 64);		/* 64*1100/256 = 275 mV */
		check("pin tracks DATA with OUTEN", g_dac_pin, 275);
		check("internal OUT also tracks DATA", g_dac_out, 275);

		cpu_write(m, D + DACR_CTRLA, ENABLE);		/* drop OUTEN */
		check("pin released to 0 when OUTEN cleared", g_dac_pin, 0);
		check("internal OUT unaffected by OUTEN clear", g_dac_out, 275);
	}

	printf("== modern DAC0 -> AC0 routing (sim_tiny3217) ==\n");
	{
		const avr_io_addr_t C = 0x680, D = 0x6a0;
		enum { AC_ENABLE = 0x01, DAC_ENABLE = 0x01 };
		enum { MUXNEG_DAC = 0x03 };
		enum { STATE = 0x10 };

		avr_t *m = avr_make_mcu_by_name("attiny3217");
		if (!m) { printf("cannot make attiny3217 core\n"); return 2; }
		m->log = LOG_ERROR;
		avr_init(m);
		memset(m->flash, 0, 0x2000);

		/* AC0: V+ = AINP0 = 800 mV, V- = DAC output. */
		avr_raise_irq(avr_io_getirq(m, AVR_IOCTL_AC_GETIRQ('0'), AVR_AC_IRQ_AINP0), 800);
		cpu_write(m, C + ACR_MUXCTRLA, MUXNEG_DAC);
		cpu_write(m, C + ACR_CTRLA, AC_ENABLE);

		/* DAC = 128 -> 550 mV; 800 > 550 => STATE 1. */
		cpu_write(m, D + DACR_CTRLA, DAC_ENABLE);
		cpu_write(m, D + DACR_DATA, 128);
		check("AC STATE 1 (800 > DAC 550)", !!(cpu_read(m, C + ACR_STATUS) & STATE), 1);

		/* DAC = 255 -> 1095 mV; 800 < 1095 => STATE 0. */
		cpu_write(m, D + DACR_DATA, 255);
		check("AC STATE 0 (800 < DAC 1095)", !!(cpu_read(m, C + ACR_STATUS) & STATE), 0);
	}

	printf("== modern DAC1/DAC2 -> AC1/AC2 routing (sim_tiny3217) ==\n");
	{
		/* The 16K/32K tinyAVR-1 parts fit DAC1 (0x06A8) and DAC2 (0x06B0). They
		 * have no output pin; the DATA-scaled output is the AC1/AC2 DAC negative
		 * input (DS40002205A 31.3.2.3, Table 7-1). VREF.CTRLC/CTRLD select the
		 * DAC1/DAC2 reference. */
		const avr_io_addr_t V = 0xa0, C1 = 0x688, D1 = 0x6a8, C2 = 0x690, D2 = 0x6b0;
		enum { CTRLC = 0x02, CTRLD = 0x03 };
		enum { AC_ENABLE = 0x01, DAC_ENABLE = 0x01, MUXNEG_DAC = 0x03, STATE = 0x10 };

		avr_t *m = avr_make_mcu_by_name("attiny3217");
		if (!m) { printf("cannot make attiny3217 core\n"); return 2; }
		m->log = LOG_ERROR;
		avr_init(m);
		memset(m->flash, 0, 0x2000);

		/* DAC1 reference = 1100 mV (CTRLC.DAC1REFSEL = 1). AC1: V+ = 800 mV,
		 * V- = DAC1. DATA = 128 -> 550 mV; 800 > 550 => STATE 1. */
		cpu_write(m, V + CTRLC, 0x01);
		avr_raise_irq(avr_io_getirq(m, AVR_IOCTL_AC_GETIRQ('1'), AVR_AC_IRQ_AINP0), 800);
		cpu_write(m, C1 + ACR_MUXCTRLA, MUXNEG_DAC);
		cpu_write(m, C1 + ACR_CTRLA, AC_ENABLE);
		cpu_write(m, D1 + DACR_CTRLA, DAC_ENABLE);
		cpu_write(m, D1 + DACR_DATA, 128);
		check("AC1 STATE 1 (800 > DAC1 550)", !!(cpu_read(m, C1 + ACR_STATUS) & STATE), 1);
		/* DATA = 255 -> 1095 mV; 800 < 1095 => STATE 0 (DATA scaling reaches AC1). */
		cpu_write(m, D1 + DACR_DATA, 255);
		check("AC1 STATE 0 (800 < DAC1 1095)", !!(cpu_read(m, C1 + ACR_STATUS) & STATE), 0);

		/* DAC2 reference = 1100 mV (CTRLD.DAC2REFSEL = 1). AC2: V+ = 800 mV,
		 * V- = DAC2; DATA = 128 -> 550 mV => STATE 1, DATA = 255 => STATE 0. */
		cpu_write(m, V + CTRLD, 0x01);
		avr_raise_irq(avr_io_getirq(m, AVR_IOCTL_AC_GETIRQ('2'), AVR_AC_IRQ_AINP0), 800);
		cpu_write(m, C2 + ACR_MUXCTRLA, MUXNEG_DAC);
		cpu_write(m, C2 + ACR_CTRLA, AC_ENABLE);
		cpu_write(m, D2 + DACR_CTRLA, DAC_ENABLE);
		cpu_write(m, D2 + DACR_DATA, 128);
		check("AC2 STATE 1 (800 > DAC2 550)", !!(cpu_read(m, C2 + ACR_STATUS) & STATE), 1);
		cpu_write(m, D2 + DACR_DATA, 255);
		check("AC2 STATE 0 (800 < DAC2 1095)", !!(cpu_read(m, C2 + ACR_STATUS) & STATE), 0);
	}

	printf("== modern AC0 -> CCL input source (sim_tiny3217) ==\n");
	{
		const avr_io_addr_t C = 0x680, D = 0x6a0, L = 0x1c0;
		enum { AC_ENABLE = 0x01, DAC_ENABLE = 0x01, MUXNEG_DAC = 0x03 };
		enum { CTRLA = 0x00, L0CTRLA = 0x05, L0CTRLB = 0x06, L0CTRLC = 0x07,
			   TRUTH0 = 0x08 };
		enum { CCL_EN = 0x01, LUT_EN = 0x01 };

		avr_t *m = avr_make_mcu_by_name("attiny3217");
		if (!m) { printf("cannot make attiny3217 core\n"); return 2; }
		m->log = LOG_ERROR;
		avr_init(m);
		memset(m->flash, 0, 0x2000);

		uint8_t l0 = 0;
		avr_irq_register_notify(
			avr_io_getirq(m, AVR_IOCTL_CCL_GETIRQ('0'), AVR_CCL_IRQ_LUT0_OUT),
			rec_irq, &l0);

		/* LUT0 = pass-through of AC0 (INSEL0 = 0x6). */
		cpu_write(m, L + L0CTRLB, 0x06);
		cpu_write(m, L + L0CTRLC, 0x00);
		cpu_write(m, L + TRUTH0, 0xaa);		/* OUT = IN0 */
		cpu_write(m, L + L0CTRLA, LUT_EN);
		cpu_write(m, L + CTRLA, CCL_EN);

		/* AC0: V+ = AINP0 = 800 mV, V- = DAC. The AC OUT IRQ is wired to the
		 * CCL AC0 source by the core, so the LUT follows the comparator. */
		avr_raise_irq(avr_io_getirq(m, AVR_IOCTL_AC_GETIRQ('0'), AVR_AC_IRQ_AINP0), 800);
		cpu_write(m, C + ACR_MUXCTRLA, MUXNEG_DAC);
		cpu_write(m, C + ACR_CTRLA, AC_ENABLE);
		cpu_write(m, D + DACR_CTRLA, DAC_ENABLE);
		cpu_write(m, D + DACR_DATA, 128);	/* 550 mV; 800 > 550 => AC OUT 1 */
		check("CCL LUT0 follows AC0 high (real comparator)", l0, 1);
		cpu_write(m, D + DACR_DATA, 255);	/* 1095 mV; 800 < 1095 => AC OUT 0 */
		check("CCL LUT0 follows AC0 low (real comparator)", l0, 0);
	}

	printf("== modern CCL (sim_tiny3217) ==\n");
		{
			const avr_io_addr_t L = 0x1c0;
			enum { CTRLA = 0x00, SEQCTRL0 = 0x01,
				   L0CTRLA = 0x05, L0CTRLB = 0x06, L0CTRLC = 0x07,
				   TRUTH0 = 0x08, L1CTRLA = 0x09, L1CTRLB = 0x0a, L1CTRLC = 0x0b,
				   TRUTH1 = 0x0c };
			enum { CCL_EN = 0x01, LUT_EN = 0x01, FILT_SYNCH = 0x10, EDGEDET = 0x80,
				   CLKSRC_IN2 = 0x40 };
			/* INSEL: MASK=0, LINK=2, IO=5. */

		avr_t *m = avr_make_mcu_by_name("attiny3217");
		if (!m) { printf("cannot make attiny3217 core\n"); return 2; }
		m->log = LOG_ERROR;
		avr_init(m);
		memset(m->flash, 0, 0x2000);

		uint8_t l0 = 0, l1 = 0;
		avr_irq_register_notify(
			avr_io_getirq(m, AVR_IOCTL_CCL_GETIRQ('0'), AVR_CCL_IRQ_LUT0_OUT),
			rec_irq, &l0);
		avr_irq_register_notify(
			avr_io_getirq(m, AVR_IOCTL_CCL_GETIRQ('0'), AVR_CCL_IRQ_LUT1_OUT),
			rec_irq, &l1);
		avr_irq_t *in0 = avr_io_getirq(m, AVR_IOCTL_CCL_GETIRQ('0'), AVR_CCL_IRQ_LUT0_IN0);
		avr_irq_t *in1 = avr_io_getirq(m, AVR_IOCTL_CCL_GETIRQ('0'), AVR_CCL_IRQ_LUT0_IN1);

		/* LUT0 = (IN0 AND IN1), IN2 masked. TRUTH bit at idx 0b011 => 0x08. */
		cpu_write(m, L + L0CTRLB, (5 << 4) | 5);	/* IN1=IO, IN0=IO */
		cpu_write(m, L + L0CTRLC, 0x00);		/* IN2=MASK */
		cpu_write(m, L + TRUTH0, 0x08);
		cpu_write(m, L + L0CTRLA, LUT_EN);
		cpu_write(m, L + CTRLA, CCL_EN);

		avr_raise_irq(in0, 1); avr_raise_irq(in1, 0);
		check("AND(1,0) = 0", l0, 0);
		avr_raise_irq(in1, 1);
		check("AND(1,1) = 1", l0, 1);
		avr_raise_irq(in0, 0);
		check("AND(0,1) = 0", l0, 0);

		/* LUT1 = NOT(LINK from LUT0): IN0=LINK, others masked.
		 * out = !in0 => TRUTH bits at even idx (0,2,4,6) = 0x55. */
		cpu_write(m, L + L1CTRLB, 0x02);	/* IN0 = LINK (LUT0 output) */
		cpu_write(m, L + L1CTRLC, 0x00);
		cpu_write(m, L + TRUTH1, 0x55);
		cpu_write(m, L + L1CTRLA, LUT_EN);

		/* LUT0 currently 0 (AND(0,1)) => LUT1 = NOT 0 = 1. */
		check("LUT1 = NOT(LUT0=0) = 1", l1, 1);
		/* Drive LUT0 to 1 => LUT1 follows to 0 (combinational LINK settles). */
		avr_raise_irq(in0, 1);
		check("LUT0 AND(1,1) = 1", l0, 1);
		check("LUT1 = NOT(LUT0=1) = 0", l1, 0);

			/* Disabling the CCL forces all LUT outputs to 0. */
			cpu_write(m, L + CTRLA, 0x00);
			check("LUT0 forced 0 when CCL disabled", l0, 0);
			check("LUT1 forced 0 when CCL disabled", l1, 0);

			/* Synchronizer path clocked from IN2: two rising edges of IN2 are
			 * required before the visible output follows IN0. */
			avr_irq_t *clk0 = avr_io_getirq(m, AVR_IOCTL_CCL_GETIRQ('0'), AVR_CCL_IRQ_LUT0_IN2);
			cpu_write(m, L + L0CTRLB, 0x05);		/* IN0=IO, IN1=MASK */
			cpu_write(m, L + L0CTRLC, 0x05);		/* IN2=IO (clock source) */
			cpu_write(m, L + TRUTH0, 0x0a);		/* OUT = IN0 (IN2 masked in truth) */
			cpu_write(m, L + L0CTRLA, LUT_EN | FILT_SYNCH | CLKSRC_IN2);
			cpu_write(m, L + CTRLA, CCL_EN);
			avr_raise_irq(in0, 1);
			check("SYNC holds old state before clock", l0, 0);
			avr_raise_irq(clk0, 1);
			check("SYNC still delayed after 1st edge", l0, 0);
			avr_raise_irq(clk0, 0);
			avr_raise_irq(clk0, 1);
			check("SYNC updates after 2nd edge", l0, 1);
			avr_raise_irq(clk0, 0);
			avr_raise_irq(in0, 0);
			avr_raise_irq(clk0, 1);
			check("SYNC holds previous high after 1st falling edge", l0, 1);
			avr_raise_irq(clk0, 0);
			avr_raise_irq(clk0, 1);
			check("SYNC updates low after 2nd falling edge", l0, 0);
			avr_raise_irq(clk0, 0);

			/* Edge detector emits a one-clock pulse on the synchronized rising edge. */
			cpu_write(m, L + L0CTRLA, LUT_EN | FILT_SYNCH | EDGEDET | CLKSRC_IN2);
			avr_raise_irq(in0, 1);
			avr_raise_irq(clk0, 1);
			check("EDGEDET pulse not yet emitted after 1st edge", l0, 0);
			avr_raise_irq(clk0, 0);
			avr_raise_irq(clk0, 1);
			check("EDGEDET emits pulse on rising synchronized edge", l0, 1);
			avr_raise_irq(clk0, 0);
			avr_raise_irq(clk0, 1);
			check("EDGEDET pulse clears on next clock", l0, 0);
			avr_raise_irq(clk0, 0);
			avr_raise_irq(in0, 0);

			/* FEEDBACK is the pair sequencer output. Use a DFF whose D input is
			 * NOT(FEEDBACK), clocked by IN2, so each clock edge toggles the state. */
			cpu_write(m, L + L0CTRLB, 0x01);		/* IN0=FEEDBACK */
			cpu_write(m, L + L0CTRLC, 0x05);		/* IN2=IO clock */
			cpu_write(m, L + TRUTH0, 0x55);		/* OUT = NOT(IN0) */
			cpu_write(m, L + L0CTRLA, LUT_EN | CLKSRC_IN2);
			cpu_write(m, L + L1CTRLB, 0x00);
			cpu_write(m, L + L1CTRLC, 0x00);
			cpu_write(m, L + TRUTH1, 0xff);		/* odd LUT gate = 1 */
			cpu_write(m, L + L1CTRLA, LUT_EN);
			cpu_write(m, L + SEQCTRL0, 0x01);		/* DFF */
			cpu_write(m, L + CTRLA, CCL_EN);
			avr_raise_irq(clk0, 0);			/* start from a known low clock level */
			check("FEEDBACK starts from cleared sequencer state", l0, 1);
			avr_raise_irq(clk0, 1);
			check("FEEDBACK toggles low after 1st clock", l0, 0);
			avr_raise_irq(clk0, 0);
			avr_raise_irq(clk0, 1);
			check("FEEDBACK toggles high after 2nd clock", l0, 1);

			/* Event/peripheral input sources (tinyAVR-1 INSEL map). A source
			 * IRQ drives the cached level; the LUT (OUT=IN0) follows it. */
			avr_irq_t *s_ac0 = avr_io_getirq(m, AVR_IOCTL_CCL_GETIRQ('0'),
				AVR_CCL_IRQ_SRC_2LUT(AVR_CCL_SRC_AC0));
			avr_irq_t *s_ev0 = avr_io_getirq(m, AVR_IOCTL_CCL_GETIRQ('0'),
				AVR_CCL_IRQ_SRC_2LUT(AVR_CCL_SRC_EVENT0));
			avr_irq_t *s_ac2 = avr_io_getirq(m, AVR_IOCTL_CCL_GETIRQ('0'),
				AVR_CCL_IRQ_SRC_2LUT(AVR_CCL_SRC_AC2));
			avr_irq_t *s_tca1 = avr_io_getirq(m, AVR_IOCTL_CCL_GETIRQ('0'),
				AVR_CCL_IRQ_SRC_2LUT(AVR_CCL_SRC_TCA0_WO1));

			cpu_write(m, L + CTRLA, 0x00);		/* clean reconfigure */
			cpu_write(m, L + SEQCTRL0, 0x00);
			cpu_write(m, L + L1CTRLA, 0x00);
			cpu_write(m, L + L0CTRLC, 0x00);	/* IN2 = MASK */
			cpu_write(m, L + TRUTH0, 0xaa);		/* OUT = IN0 */

			cpu_write(m, L + L0CTRLB, 0x06);	/* IN0 = AC0 (0x6) */
			cpu_write(m, L + L0CTRLA, LUT_EN);
			cpu_write(m, L + CTRLA, CCL_EN);
			avr_raise_irq(s_ac0, 1);
			check("tiny CCL IN0=AC0 follows source high", l0, 1);
			avr_raise_irq(s_ac0, 0);
			check("tiny CCL IN0=AC0 follows source low", l0, 0);

			cpu_write(m, L + CTRLA, 0x00);
			cpu_write(m, L + L0CTRLB, 0x03);	/* IN0 = EVENT0 (0x3) */
			cpu_write(m, L + CTRLA, CCL_EN);
			avr_raise_irq(s_ev0, 1);
			check("tiny CCL IN0=EVENT0 follows event high", l0, 1);
			avr_raise_irq(s_ev0, 0);

			cpu_write(m, L + CTRLA, 0x00);
			cpu_write(m, L + L0CTRLB, 0x0e);	/* IN0 = AC2 (0xE, tiny-only) */
			cpu_write(m, L + CTRLA, CCL_EN);
			avr_raise_irq(s_ac2, 1);
			check("tiny CCL IN0=AC2 (high value) follows source", l0, 1);
			avr_raise_irq(s_ac2, 0);

			/* Position-dependent decode: INSEL=0x8 on IN1 selects TCA0 WO1. */
			cpu_write(m, L + CTRLA, 0x00);
			cpu_write(m, L + L0CTRLB, (0x8 << 4) | 0x0);	/* IN1=TCA0, IN0=MASK */
			cpu_write(m, L + TRUTH0, 0xcc);			/* OUT = IN1 */
			cpu_write(m, L + CTRLA, CCL_EN);
			avr_raise_irq(s_tca1, 1);
			check("tiny CCL IN1=TCA0 selects WO1 (position decode)", l0, 1);
			avr_raise_irq(s_tca1, 0);
			check("tiny CCL IN1=TCA0 WO1 follows low", l0, 0);
		}

	printf("== modern EVSYS (sim_tiny3217) ==\n");
	{
		const avr_io_addr_t E = 0x180;
		enum { ASYNCUSER0 = 0x12, ASYNCUSER1 = 0x13, ASYNCUSER2 = 0x14,
			   SYNCUSER0 = 0x22, SYNCSTROBE = 0x01 };
		enum { SEL_ASYNCCH0 = 3, SEL_SYNCCH0 = 1 };	/* user channel-select values */

		avr_t *m = avr_make_mcu_by_name("attiny3217");
		if (!m) { printf("cannot make attiny3217 core\n"); return 2; }
		m->log = LOG_ERROR;
		avr_init(m);
		memset(m->flash, 0, 0x2000);

		uint8_t u0 = 0, u1 = 0, u2 = 0;
		avr_irq_register_notify(
			avr_io_getirq(m, AVR_IOCTL_EVSYS_GETIRQ('0'), AVR_EVSYS_IRQ_USER0 + 0), rec_irq, &u0);
		avr_irq_register_notify(
			avr_io_getirq(m, AVR_IOCTL_EVSYS_GETIRQ('0'), AVR_EVSYS_IRQ_USER0 + 1), rec_irq, &u1);
		avr_irq_register_notify(
			avr_io_getirq(m, AVR_IOCTL_EVSYS_GETIRQ('0'), AVR_EVSYS_IRQ_USER0 + 2), rec_irq, &u2);
		/* ASYNCCH0 == channel index 2. */
		avr_irq_t *ch2 = avr_io_getirq(m, AVR_IOCTL_EVSYS_GETIRQ('0'), AVR_EVSYS_IRQ_CH0 + 2);

		/* Route ASYNCUSER0 -> ASYNCCH0, then drive the channel level. */
		cpu_write(m, E + ASYNCUSER0, SEL_ASYNCCH0);
		avr_raise_irq(ch2, 1);
		check("USER0 follows channel high", u0, 1);
		avr_raise_irq(ch2, 0);
		check("USER0 follows channel low", u0, 0);

		/* Fan-out: a second user on the same channel sees it too. */
		cpu_write(m, E + ASYNCUSER1, SEL_ASYNCCH0);
		avr_raise_irq(ch2, 1);
		check("USER0 high (fan-out)", u0, 1);
		check("USER1 high (fan-out)", u1, 1);

		/* Re-routing delivers the channel's current level immediately. */
		cpu_write(m, E + ASYNCUSER2, SEL_ASYNCCH0);	/* channel is currently 1 */
		check("re-routed USER2 gets current level", u2, 1);

		/* Turning a user off stops delivery. */
		cpu_write(m, E + ASYNCUSER0, 0x00);
		u0 = 0xaa;	/* sentinel */
		avr_raise_irq(ch2, 0);
		avr_raise_irq(ch2, 1);
		check("USER0 off receives nothing", u0, 0xaa);
		check("USER1 still receiving", u1, 1);

		/* Software strobe pulses a sync channel to its users. */
		g_evsys_pulses = 0;
		avr_irq_register_notify(
			avr_io_getirq(m, AVR_IOCTL_EVSYS_GETIRQ('0'), AVR_EVSYS_IRQ_USER0 + 13),
			evsys_count_hook, NULL);
		cpu_write(m, E + SYNCUSER0, SEL_SYNCCH0);	/* SYNCUSER0 -> SYNCCH0 (ch0) */
		cpu_write(m, E + SYNCSTROBE, 0x01);		/* strobe SYNCCH0 */
		check("sync strobe pulses the routed user once", g_evsys_pulses, 1);
	}

	printf("== modern EVSYS end-to-end: AC0 -> channel -> ADC0 start (sim_tiny3217) ==\n");
	{
		const avr_io_addr_t E = 0x180, C = 0x680, A = 0x600;
		enum { ASYNCCH0 = 0x02, ASYNCUSER1 = 0x13 };	/* ADC0 user */
		enum { AC_ENABLE = 0x01, MUXNEG_VREF = 0x02 };
		enum { GEN_AC0_OUT = 0x03, SEL_ASYNCCH0 = 3 };
		enum { ADC_ENABLE = 0x01, STCONV = 0x01, STARTEI = 0x01 };
		enum { F_RESRDY = 0x01 };

		avr_t *m = avr_make_mcu_by_name("attiny3217");
		if (!m) { printf("cannot make attiny3217 core\n"); return 2; }
		m->log = LOG_ERROR;
		avr_init(m);
		memset(m->flash, 0, 0x2000);

		/* AC0: V+ = AINP0, V- = internal VREF (1100 mV). Start with V+ low so the
		 * comparator output is 0. */
		avr_irq_t *ainp0 = avr_io_getirq(m, AVR_IOCTL_AC_GETIRQ('0'), AVR_AC_IRQ_AINP0);
		avr_raise_irq(ainp0, 500);
		cpu_write(m, C + ACR_MUXCTRLA, MUXNEG_VREF);
		cpu_write(m, C + ACR_CTRLA, AC_ENABLE);

		/* Route AC0 output through ASYNCCH0 to the ADC0 event user. */
		cpu_write(m, E + ASYNCCH0, GEN_AC0_OUT);	/* channel source = AC0_OUT */
		cpu_write(m, E + ASYNCUSER1, SEL_ASYNCCH0);	/* ADC0 user <- ASYNCCH0 */

		/* ADC0: channel 0 = 1650 mV (=> 512), event-triggered start armed. */
		avr_raise_irq(avr_io_getirq(m, AVR_IOCTL_ADCM_GETIRQ('0'), 0), 1650);
		cpu_write(m, A + ADCMR_MUXPOS, 0);
		cpu_write(m, A + ADCMR_EVCTRL, STARTEI);
		cpu_write(m, A + ADCMR_CTRLA, ADC_ENABLE);
		cpu_write(m, A + ADCMR_INTFLAGS, F_RESRDY);	/* clear */

		check("no conversion before event", !!(m->data[A + ADCMR_INTFLAGS] & F_RESRDY), 0);

		/* Raise AINP0 above VREF: AC0 output 0->1 generates the event, EVSYS
		 * routes it to the ADC0 user, which starts a conversion. */
		avr_raise_irq(ainp0, 2000);
		for (int i = 0; i < 400 && !(m->data[A + ADCMR_INTFLAGS] & F_RESRDY); i++)
			avr_run(m);
		check("AC0 event started an ADC conversion",
			  !!(m->data[A + ADCMR_INTFLAGS] & F_RESRDY), 1);
		check("event-triggered result = 512",
			  (cpu_read(m, A + ADCMR_RESL) | (cpu_read(m, A + ADCMR_RESH) << 8)), 512);

		/* With STARTEI cleared, a fresh AC0 event must NOT start a conversion. */
		cpu_write(m, A + ADCMR_EVCTRL, 0x00);
		cpu_write(m, A + ADCMR_INTFLAGS, F_RESRDY);	/* clear */
		avr_raise_irq(ainp0, 500);			/* AC0 out 1->0 */
		avr_raise_irq(ainp0, 2000);			/* AC0 out 0->1 (event) */
		for (int i = 0; i < 400; i++) avr_run(m);
		check("no conversion when STARTEI cleared",
			  !!(m->data[A + ADCMR_INTFLAGS] & F_RESRDY), 0);
	}

	printf("== modern PORTMUX (sim_tiny3217) ==\n");
	{
		const avr_io_addr_t P = 0x200;

		avr_t *m = avr_make_mcu_by_name("attiny3217");
		if (!m) { printf("cannot make attiny3217 core\n"); return 2; }
		m->log = LOG_ERROR;
		avr_init(m);

		/* Default routing: all CTRL registers reset to 0. */
		check("PORTMUX CTRLA reset 0", cpu_read(m, P + PORTMUXR_CTRLA), 0);
		check("PORTMUX CTRLB reset 0", cpu_read(m, P + PORTMUXR_CTRLB), 0);

		/* Routing selections store and read back (no behavioural effect). */
		cpu_write(m, P + PORTMUXR_CTRLB, 0x15);	/* USART0 | SPI0 | TWI0 alt */
		check("PORTMUX CTRLB stores selection", cpu_read(m, P + PORTMUXR_CTRLB), 0x15);
		cpu_write(m, P + PORTMUXR_CTRLC, 0x2a);	/* TCA0 WO alt set */
		check("PORTMUX CTRLC stores selection", cpu_read(m, P + PORTMUXR_CTRLC), 0x2a);
		cpu_write(m, P + PORTMUXR_CTRLA, 0x30);	/* LUT0 | LUT1 alt */
		check("PORTMUX CTRLA stores selection", cpu_read(m, P + PORTMUXR_CTRLA), 0x30);

		/* A neighbouring register is unaffected. */
		check("PORTMUX CTRLD untouched", cpu_read(m, P + PORTMUXR_CTRLD), 0);
	}

	printf("== modern VREF (sim_tiny3217) ==\n");
	{
		const avr_io_addr_t V = 0x0a0, C = 0x680, D = 0x6a0;
		enum { CTRLA = 0x00, CTRLB = 0x01, CTRLC = 0x02, CTRLD = 0x03 };
		enum { AC_ENABLE = 0x01, DAC_ENABLE = 0x01 };
		enum { MUXNEG_VREF = 0x02, STATE = 0x10 };
		/* CTRLA: ADC0REFSEL[6:4], DAC0REFSEL[2:0]; codes 0..4 ->
		 * 0.55/1.1/2.5/4.3/1.5 V. */

		avr_t *m = avr_make_mcu_by_name("attiny3217");
		if (!m) { printf("cannot make attiny3217 core\n"); return 2; }
		m->log = LOG_ERROR;
		avr_init(m);
		memset(m->flash, 0, 0x2000);

		/* The decode helper maps the five defined reference selections. */
		check("VREF sel 0 -> 550 mV", avr_vref_sel_to_mv(0), 550);
		check("VREF sel 1 -> 1100 mV", avr_vref_sel_to_mv(1), 1100);
		check("VREF sel 2 -> 2500 mV", avr_vref_sel_to_mv(2), 2500);
		check("VREF sel 3 -> 4300 mV", avr_vref_sel_to_mv(3), 4300);
		check("VREF sel 4 -> 1500 mV", avr_vref_sel_to_mv(4), 1500);
		check("VREF sel 5 reserved -> 0", avr_vref_sel_to_mv(5), 0);

		/* All CTRL registers reset to 0. */
		check("VREF CTRLA reset 0", cpu_read(m, V + CTRLA), 0);
		check("VREF CTRLB reset 0", cpu_read(m, V + CTRLB), 0);

		/* CTRLB force-enable bits and CTRLC/CTRLD store and read back. */
		cpu_write(m, V + CTRLB, 0x03);	/* ADC0REFEN | DAC0REFEN */
		check("VREF CTRLB stores force-enable", cpu_read(m, V + CTRLB), 0x03);
		cpu_write(m, V + CTRLC, 0x12);
		check("VREF CTRLC stores", cpu_read(m, V + CTRLC), 0x12);
		cpu_write(m, V + CTRLD, 0x04);
		check("VREF CTRLD stores", cpu_read(m, V + CTRLD), 0x04);

		/* CTRLA stores its selection and drives DAC0/AC0's reference. AC0 with
		 * V+ = AINP0 = 1200 mV, V- = internal VREF lets us observe the change. */
		avr_raise_irq(avr_io_getirq(m, AVR_IOCTL_AC_GETIRQ('0'), AVR_AC_IRQ_AINP0), 1200);
		cpu_write(m, C + ACR_MUXCTRLA, MUXNEG_VREF);
		cpu_write(m, C + ACR_CTRLA, AC_ENABLE);

		/* DAC0REFSEL = 1 (1.1V), ADC0REFSEL = 2 (2.5V). */
		cpu_write(m, V + CTRLA, (0x2 << 4) | 0x1);
		check("VREF CTRLA stores selection", cpu_read(m, V + CTRLA), 0x21);
		/* AC0 VREF now 1100 mV: 1200 > 1100 => STATE 1. */
		check("AC STATE 1 (1200 > VREF 1100)",
				!!(cpu_read(m, C + ACR_STATUS) & STATE), 1);

		/* Re-select DAC0REFSEL = 3 (4.3V): 1200 < 4300 => STATE 0. */
		cpu_write(m, V + CTRLA, 0x03);
		check("AC STATE 0 (1200 < VREF 4300)",
				!!(cpu_read(m, C + ACR_STATUS) & STATE), 0);

		/* DAC0 also picks up the selected reference: with DAC0REFSEL = 1
		 * (1100 mV), DATA = 128 -> 128 * 1100 / 256 = 550 mV. */
		g_dac_out = 0xffffffff;
		avr_irq_register_notify(
			avr_io_getirq(m, AVR_IOCTL_DAC_GETIRQ('0'), AVR_DAC_IRQ_OUT),
			dac_capture_hook, NULL);
		cpu_write(m, V + CTRLA, 0x01);		/* DAC0REFSEL = 1 -> 1100 mV */
		cpu_write(m, D + DACR_CTRLA, DAC_ENABLE);
		cpu_write(m, D + DACR_DATA, 128);
		check("DAC out 550 mV with VREF 1100 (DATA=128)", g_dac_out, 550);

		/* CTRLC.DAC1REFSEL[2:0] selects the DAC1/AC1 reference, also feeding AC1's
		 * internal-VREF input (MUXNEG=2); CTRLC.ADC1REFSEL[6:4] drives ADC1's
		 * internal reference (16K/32K). The DAC-input (MUXNEG=3) path is exercised
		 * separately below. AC1 at 0x688: V+ = AINP0 = 1200 mV, V- = internal VREF. */
		const avr_io_addr_t C1 = 0x688, C2 = 0x690, A1 = 0x640;
		avr_raise_irq(avr_io_getirq(m, AVR_IOCTL_AC_GETIRQ('1'), AVR_AC_IRQ_AINP0), 1200);
		cpu_write(m, C1 + ACR_MUXCTRLA, MUXNEG_VREF);
		cpu_write(m, C1 + ACR_CTRLA, AC_ENABLE);
		cpu_write(m, V + CTRLC, 0x01);		/* DAC1REFSEL = 1 -> 1100 mV */
		check("AC1 STATE 1 (1200 > CTRLC VREF 1100)",
				!!(cpu_read(m, C1 + ACR_STATUS) & STATE), 1);
		cpu_write(m, V + CTRLC, 0x03);		/* DAC1REFSEL = 3 -> 4300 mV */
		check("AC1 STATE 0 (1200 < CTRLC VREF 4300)",
				!!(cpu_read(m, C1 + ACR_STATUS) & STATE), 0);

		/* CTRLD.DAC2REFSEL[2:0] selects DAC2/AC2's reference (internal-VREF path).
		 * AC2 @0x690. */
		avr_raise_irq(avr_io_getirq(m, AVR_IOCTL_AC_GETIRQ('2'), AVR_AC_IRQ_AINP0), 1200);
		cpu_write(m, C2 + ACR_MUXCTRLA, MUXNEG_VREF);
		cpu_write(m, C2 + ACR_CTRLA, AC_ENABLE);
		cpu_write(m, V + CTRLD, 0x01);		/* DAC2REFSEL = 1 -> 1100 mV */
		check("AC2 STATE 1 (1200 > CTRLD VREF 1100)",
				!!(cpu_read(m, C2 + ACR_STATUS) & STATE), 1);
		cpu_write(m, V + CTRLD, 0x04);		/* DAC2REFSEL = 4 -> 1500 mV */
		check("AC2 STATE 0 (1200 < CTRLD VREF 1500)",
				!!(cpu_read(m, C2 + ACR_STATUS) & STATE), 0);

		/* CTRLC.ADC1REFSEL[6:4] tracks ADC1's internal reference. ADC1 @0x640,
		 * AIN = 550 mV. ADC1REFSEL = 1 (1.1 V) => 550*1024/1100 = 512. */
		avr_raise_irq(avr_io_getirq(m, AVR_IOCTL_ADCM_GETIRQ('1'), 0), 550);
		cpu_write(m, A1 + ADCMR_MUXPOS, 0);
		cpu_write(m, A1 + ADCMR_CTRLA, 0x01);	/* ENABLE; REFSEL defaults INTREF */
		cpu_write(m, V + CTRLC, (1 << 4));	/* ADC1REFSEL = 1.1V */
		cpu_write(m, A1 + ADCMR_INTFLAGS, 0x01);
		cpu_write(m, A1 + ADCMR_COMMAND, 0x01);	/* STCONV */
		for (int i = 0; i < 4000 &&
			 !(m->data[A1 + ADCMR_INTFLAGS] & 0x01); i++) avr_run(m);
		check("ADC1 INTREF tracks CTRLC VREF (1.1V) => 512",
				cpu_read(m, A1 + ADCMR_RESL) | (cpu_read(m, A1 + ADCMR_RESH) << 8),
				512);
	}

	printf("== modern TCD0 (sim_tiny3217) ==\n");
	{
		const avr_io_addr_t T = 0xa80;
		enum { ENABLE = 0x01, SYNCPRES_DIV2 = (1 << 1) };
		enum { OVF = 0x01 };
		enum { ENRDY = 0x01, CMDRDY = 0x02 };

		avr_t *m = avr_make_mcu_by_name("attiny3217");
		if (!m) { printf("cannot make attiny3217 core\n"); return 2; }
		m->log = LOG_ERROR;
		avr_init(m);
		memset(m->flash, 0, 0x2000);	/* NOPs */

		/* Default CTRLA.CLKSEL = OSC20M (unprescaled). Disable the main-clock
		 * prescaler so CLK_PER = OSC20M = 20 MHz and one TCD count == one
		 * CLK_PER cycle, keeping the cycle counts below source-independent. */
		avr_ccp_write(m, AVR_CCP_IOREG);
		cpu_write(m, 0x61, 0x00);	/* MCLKCTRLB: PEN=0 -> 20 MHz */

		/* The double-buffered sync logic always reports ready. */
		check("TCD STATUS ready (ENRDY|CMDRDY)",
			  cpu_read(m, T + TCDR_STATUS) & (ENRDY | CMDRDY), ENRDY | CMDRDY);

		/* TOP = CMPBCLR = 100 => period (TOP+1) = 101 counts, prescale 1. */
		cpu_write(m, T + TCDR_CMPBCLRL, 100);
		cpu_write(m, T + TCDR_CMPBCLRH, 0);
		cpu_write(m, T + TCDR_INTCTRL, OVF);
		long t0 = (long)m->cycle;
		cpu_write(m, T + TCDR_CTRLA, ENABLE);	/* SYNCPRES/CNTPRES DIV1 */

		long tovf = -1;
		for (int i = 0; i < 400 && tovf < 0; i++) {
			avr_run(m);
			if (m->data[T + TCDR_INTFLAGS] & OVF) tovf = (long)m->cycle;
		}
		check("OVF near 101 cycles", (tovf - t0) >= 98 && (tovf - t0) <= 106, 1);
		check("TCD raises (enabled) interrupt", avr_has_pending_interrupts(m), 1);

		/* W1C and confirm the periodic cadence (~101 cycles later). */
		cpu_write(m, T + TCDR_INTFLAGS, OVF);
		check("OVF cleared by W1C", !!(m->data[T + TCDR_INTFLAGS] & OVF), 0);
		long t1 = -1;
		for (int i = 0; i < 400 && t1 < 0; i++) {
			avr_run(m);
			if (m->data[T + TCDR_INTFLAGS] & OVF) t1 = (long)m->cycle;
		}
		check("second OVF ~101 cycles later", (t1 - tovf) >= 98 && (t1 - tovf) <= 106, 1);

		/* Disable: no further overflows. */
		cpu_write(m, T + TCDR_INTFLAGS, OVF);
		cpu_write(m, T + TCDR_CTRLA, 0x00);
		int fired = 0;
		for (int i = 0; i < 400; i++) {
			avr_run(m);
			if (m->data[T + TCDR_INTFLAGS] & OVF) { fired = 1; break; }
		}
		check("no OVF after disable", fired, 0);

		/* SYNCPRES = DIV2 doubles the period (~202 cycles). */
		cpu_write(m, T + TCDR_INTFLAGS, OVF);
		long base = (long)m->cycle, t2 = -1;
		cpu_write(m, T + TCDR_CTRLA, ENABLE | SYNCPRES_DIV2);
		for (int i = 0; i < 700 && t2 < 0; i++) {
			avr_run(m);
			if (m->data[T + TCDR_INTFLAGS] & OVF) t2 = (long)m->cycle;
		}
		check("SYNCPRES/2 period ~202 cycles", (t2 - base) >= 198 && (t2 - base) <= 208, 1);
	}

	printf("== modern TCD0 clock source (CTRLA.CLKSEL) (sim_tiny3217) ==\n");
	{
		/* With the default main-clock prescaler (OSC20M/6 = 3.333 MHz CLK_PER),
		 * a SYSCLK-clocked TCD tracks CLK_PER while an OSC20M-clocked TCD runs
		 * from the unprescaled 20 MHz oscillator -> ~6x faster. Use CNTPRES=32
		 * so the faster source is still representable in whole CLK_PER cycles.
		 * (DS40002205A 22.5.1 CLKSEL; 22.2.1 clock selection.) */
		const avr_io_addr_t T = 0xa80;
		enum { ENABLE = 0x01, CNTPRES_DIV32 = (2 << 3) };
		enum { CLKSEL_OSC20M = (0 << 5), CLKSEL_SYSCLK = (3 << 5) };
		enum { OVF = 0x01 };

		avr_t *m = avr_make_mcu_by_name("attiny3217");
		if (!m) { printf("cannot make attiny3217 core\n"); return 2; }
		m->log = LOG_ERROR;
		avr_init(m);
		memset(m->flash, 0, 0x2000);	/* NOPs */
		check("default CLK_PER = 20M/6", m->frequency, 3333333);

		/* TOP = 100 -> 101 counts; CNTPRES=32 -> 32 source ticks per count. */
		cpu_write(m, T + TCDR_CMPBCLRL, 100);
		cpu_write(m, T + TCDR_CMPBCLRH, 0);
		cpu_write(m, T + TCDR_INTCTRL, OVF);

		/* SYSCLK: period = 101 * 32 CLK_PER cycles = 3232. */
		long s0 = (long)m->cycle, sovf = -1;
		cpu_write(m, T + TCDR_CTRLA, ENABLE | CNTPRES_DIV32 | CLKSEL_SYSCLK);
		for (int i = 0; i < 4000 && sovf < 0; i++) {
			avr_run(m);
			if (m->data[T + TCDR_INTFLAGS] & OVF) sovf = (long)m->cycle;
		}
		check("SYSCLK period ~3232 cycles",
			  (sovf - s0) >= 3200 && (sovf - s0) <= 3264, 1);

		/* Restart on OSC20M: 20 MHz / 3.333 MHz = 6x faster.
		 * 101 * round(32/6=5.33)=5 = 505 CLK_PER cycles. */
		cpu_write(m, T + TCDR_CTRLA, 0x00);
		cpu_write(m, T + TCDR_INTFLAGS, OVF);
		long o0 = (long)m->cycle, oovf = -1;
		cpu_write(m, T + TCDR_CTRLA, ENABLE | CNTPRES_DIV32 | CLKSEL_OSC20M);
		for (int i = 0; i < 4000 && oovf < 0; i++) {
			avr_run(m);
			if (m->data[T + TCDR_INTFLAGS] & OVF) oovf = (long)m->cycle;
		}
		check("OSC20M period ~505 cycles (6x faster than SYSCLK)",
			  (oovf - o0) >= 480 && (oovf - o0) <= 530, 1);
	}

	printf("== modern TCD0 waveform output / One Ramp PWM (sim_tiny3217) ==\n");
	{
		const avr_io_addr_t T = 0xa80;
		enum { ENABLE = 0x01 };
		enum { CMPAEN = 0x10, CMPBEN = 0x20 };

		avr_t *m = avr_make_mcu_by_name("attiny3217");
		if (!m) { printf("cannot make attiny3217 core\n"); return 2; }
		m->log = LOG_ERROR;
		avr_init(m);
		memset(m->flash, 0, 0x2000);	/* NOPs */

		uint8_t woa = 0xff, wob = 0xff;
		avr_irq_register_notify(
			avr_io_getirq(m, AVR_IOCTL_TCD_GETIRQ('0'), AVR_TCD_IRQ_WOA), rec_irq, &woa);
		avr_irq_register_notify(
			avr_io_getirq(m, AVR_IOCTL_TCD_GETIRQ('0'), AVR_TCD_IRQ_WOB), rec_irq, &wob);

		/* Route TCD0 WOA into CCL LUT0 (INSEL 0x9 on IN0, pass-through); the
		 * core wires TCD WOA->CCL, so the LUT output tracks WOA. */
		uint8_t ccl_woa = 0;
		avr_irq_register_notify(
			avr_io_getirq(m, AVR_IOCTL_CCL_GETIRQ('0'), AVR_CCL_IRQ_LUT0_OUT),
			rec_irq, &ccl_woa);
		cpu_write(m, 0x1c0 + 0x06, 0x09);	/* L0CTRLB: IN0 = TCD0 (WOA) */
		cpu_write(m, 0x1c0 + 0x07, 0x00);	/* L0CTRLC: IN2 mask */
		cpu_write(m, 0x1c0 + 0x08, 0xaa);	/* TRUTH0: OUT = IN0 */
		cpu_write(m, 0x1c0 + 0x05, 0x01);	/* L0CTRLA: LUT enable */
		cpu_write(m, 0x1c0 + 0x00, 0x01);	/* CTRLA: CCL enable */

		/* One Ramp, prescale 1: TOP=99 (period 100), WOA on [10,40), WOB on
		 * [50,99). */
		cpu_write(m, T + TCDR_CMPBCLRL, 99); cpu_write(m, T + TCDR_CMPBCLRH, 0);
		cpu_write(m, T + TCDR_CMPASETL, 10); cpu_write(m, T + TCDR_CMPASETL + 1, 0);
		cpu_write(m, T + TCDR_CMPACLRL, 40); cpu_write(m, T + TCDR_CMPACLRL + 1, 0);
		cpu_write(m, T + TCDR_CMPBSETL, 50); cpu_write(m, T + TCDR_CMPBSETL + 1, 0);
		cpu_write(m, T + TCDR_FAULTCTRL, CMPAEN | CMPBEN);

		long t0 = (long)m->cycle;
		cpu_write(m, T + TCDR_CTRLA, ENABLE);

		#define RUN_TO(n) do { while ((long)m->cycle - t0 < (n)) avr_run(m); } while (0)

		/* count ~20: inside WOA on-time, before WOB. */
		RUN_TO(20);
		check("WOA high in [10,40)", woa, 1);
		check("CCL LUT0 tracks TCD0 WOA high", ccl_woa, 1);
		check("WOB still low before 50", wob, 0xff);	/* no edge yet */

		/* count ~45: WOA cleared at 40, WOB not yet set. */
		RUN_TO(45);
		check("WOA low after CMPACLR=40", woa, 0);
		check("CCL LUT0 tracks TCD0 WOA low", ccl_woa, 0);

		/* count ~60: WOB set at 50. */
		RUN_TO(60);
		check("WOB high in [50,99)", wob, 1);

		/* count ~105 (past wrap at 100): both outputs low again. */
		RUN_TO(105);
		check("WOB low after CMPBCLR=99", wob, 0);
		check("WOA low at start of new ramp", woa, 0);

		/* Periodicity: WOA rises again ~10 counts into the next ramp (~count 110). */
		RUN_TO(115);
		check("WOA high again next period", woa, 1);

		/* Disabling an output in FAULTCTRL stops driving it. */
		cpu_write(m, T + TCDR_FAULTCTRL, CMPBEN);	/* drop CMPAEN */
		woa = 0x55;					/* sentinel */
		RUN_TO(220);					/* run > a full period */
		check("WOA not driven once CMPAEN cleared", woa, 0x55);

		#undef RUN_TO
	}

	printf("== modern WDT (sim_tiny3217) ==\n");
	{
		const avr_io_addr_t W = 0x100;
		enum { CTRLA = 0x00, STATUS = 0x01 };
		enum { PERIOD_8CLK = 0x01, PERIOD_16CLK = 0x02 };
		enum { SYNCBUSY = 0x01, LOCK = 0x80 };

		/* --- CCP gating + STATUS + LOCK --- */
		avr_t *m = avr_make_mcu_by_name("attiny3217");
		if (!m) { printf("cannot make attiny3217 core\n"); return 2; }
		m->log = LOG_ERROR;
		avr_init(m);
		/* The WDT runs for tens of thousands of cycles; rather than fill flash
		 * with NOPs (the PC would overrun flashend and crash), spin on a
		 * self-looping RJMP .-2 at address 0 so cycles advance with a fixed PC. */
		put16(m, 0, 0xcfff);

		check("WDT STATUS not busy (SYNCBUSY 0)", cpu_read(m, W + STATUS) & SYNCBUSY, 0);

		/* A bare (non-CCP) CTRLA write is ignored. */
		cpu_write(m, W + CTRLA, PERIOD_8CLK);
		check("CTRLA write ignored without CCP", cpu_read(m, W + CTRLA), 0);

		/* A CCP-protected write configures the period. */
		avr_ccp_write(m, AVR_CCP_IOREG);
		cpu_write(m, W + CTRLA, PERIOD_8CLK);
		check("CTRLA set via CCP", cpu_read(m, W + CTRLA), PERIOD_8CLK);

		/* STATUS.LOCK freezes CTRLA until reset. */
		cpu_write(m, W + STATUS, LOCK);
		check("STATUS.LOCK set", !!(cpu_read(m, W + STATUS) & LOCK), 1);
		avr_ccp_write(m, AVR_CCP_IOREG);
		cpu_write(m, W + CTRLA, PERIOD_16CLK);
		check("CTRLA frozen by LOCK", cpu_read(m, W + CTRLA), PERIOD_8CLK);

		/* --- timeout resets the device --- */
		avr_t *r = avr_make_mcu_by_name("attiny3217");
		r->log = LOG_ERROR;
		avr_init(r);
		put16(r, 0, 0xcfff);	/* RJMP .-2 self-loop */

		avr_ccp_write(r, AVR_CCP_IOREG);
		cpu_write(r, W + CTRLA, PERIOD_8CLK);
		int reset_seen = 0;
		long prev = (long)r->cycle;
		for (int i = 0; i < 200000 && !reset_seen; i++) {
			avr_run(r);
			long c = (long)r->cycle;
			if (c < prev) reset_seen = 1;	/* avr_reset zeroed the cycle count */
			prev = c;
		}
		check("WDT reset fires on timeout", reset_seen, 1);

		/* --- petting (WDR) in time prevents the reset --- */
		avr_t *q = avr_make_mcu_by_name("attiny3217");
		q->log = LOG_ERROR;
		avr_init(q);
		put16(q, 0, 0xcfff);	/* RJMP .-2 self-loop */
		long Tq = 8 * (long)q->frequency / 1024;

		avr_ccp_write(q, AVR_CCP_IOREG);
		cpu_write(q, W + CTRLA, PERIOD_8CLK);
		while ((long)q->cycle < Tq * 6 / 10)	/* run to ~60% of the period */
			avr_run(q);
		avr_ioctl(q, AVR_IOCTL_WATCHDOG_RESET, 0);	/* WDR: pet the dog */
		long pet_at = (long)q->cycle, qprev = (long)q->cycle;
		int q_reset = 0;
		while ((long)q->cycle < pet_at + Tq * 8 / 10) {	/* 0.8T since pet < T */
			avr_run(q);
			long c = (long)q->cycle;
			if (c < qprev) { q_reset = 1; break; }
			qprev = c;
		}
		check("no WDT reset when petted in time", q_reset, 0);
	}

	printf("== modern CRCSCAN (sim_tiny3217) ==\n");
	{
		const avr_io_addr_t C = 0x120;
		enum { CTRLA = 0x00, STATUS = 0x02 };
		enum { ENABLE = 0x01, NMIEN = 0x02, RESET = 0x80 };
		enum { BUSY = 0x01, OK = 0x02 };
		const uint32_t FEND = 0x7fff;	/* full-flash last byte (32 KB) */

		/* The CRC helper is a real CRC-16/CCITT-FALSE (known check value). */
		check("CRC16 of \"123456789\" == 0x29B1",
			  avr_crcscan_crc16((const uint8_t *)"123456789", 9), 0x29b1);

		avr_t *m = avr_make_mcu_by_name("attiny3217");
		if (!m) { printf("cannot make attiny3217 core\n"); return 2; }
		m->log = LOG_ERROR;
		avr_init(m);	/* flash is all 0xFF */

		/* Program a valid full-flash checksum (big-endian) in the last 2 bytes. */
		uint16_t sum = avr_crcscan_crc16(m->flash, FEND - 1);
		m->flash[FEND - 1] = sum >> 8;
		m->flash[FEND] = sum & 0xff;

		/* A matching checksum: enable completes instantly with OK, never BUSY. */
		cpu_write(m, C + CTRLA, ENABLE);
		check("CRCSCAN OK on matching checksum", !!(m->data[C + STATUS] & OK), 1);
		check("CRCSCAN never BUSY", !!(m->data[C + STATUS] & BUSY), 0);

		/* Disabling clears the result. */
		cpu_write(m, C + CTRLA, 0x00);
		check("CRCSCAN OK cleared when disabled", !!(m->data[C + STATUS] & OK), 0);

		/* RESET strobe clears the result and disables (and self-clears). */
		cpu_write(m, C + CTRLA, ENABLE);
		cpu_write(m, C + CTRLA, RESET);
		check("CRCSCAN OK cleared by RESET", !!(m->data[C + STATUS] & OK), 0);
		check("CRCSCAN RESET self-clears CTRLA", m->data[C + CTRLA], 0);

		/* Corrupting a scanned byte makes the checksum mismatch: OK clears. */
		m->flash[0x100] ^= 0xff;
		cpu_write(m, C + CTRLA, ENABLE);
		check("CRCSCAN OK cleared on mismatch", !!(m->data[C + STATUS] & OK), 0);
		check("no NMI without NMIEN", avr_has_pending_interrupts(m), 0);
		cpu_write(m, C + CTRLA, RESET);
		m->flash[0x100] ^= 0xff;		/* repair */

		/* NMIEN locks CTRLA: the scan cannot be disabled until reset. */
		cpu_write(m, C + CTRLA, ENABLE | NMIEN);
		check("CRCSCAN OK with NMIEN (valid)", !!(m->data[C + STATUS] & OK), 1);
		cpu_write(m, C + CTRLA, 0x00);	/* attempt to disable */
		check("CTRLA locked by NMIEN", m->data[C + CTRLA], ENABLE | NMIEN);
		check("CRCSCAN still OK (locked)", !!(m->data[C + STATUS] & OK), 1);
	}

	printf("== modern CRCSCAN failure raises NMI (sim_tiny3217) ==\n");
	{
		const avr_io_addr_t C = 0x120;
		enum { CTRLA = 0x00, STATUS = 0x02 };
		enum { ENABLE = 0x01, NMIEN = 0x02 };
		enum { OK = 0x02 };

		avr_t *m = avr_make_mcu_by_name("attiny3217");
		if (!m) { printf("cannot make attiny3217 core\n"); return 2; }
		m->log = LOG_ERROR;
		avr_init(m);

		/* Leave a deliberately wrong checksum (0x0000) at the flash end. */
		m->flash[0x7ffe] = 0x00;
		m->flash[0x7fff] = 0x00;

		/* Enable with the NMI armed: the mismatch clears OK and raises the NMI. */
		cpu_write(m, C + CTRLA, ENABLE | NMIEN);
		check("OK cleared on CRC failure", !!(m->data[C + STATUS] & OK), 0);
		check("CRC failure raises NMI", avr_has_pending_interrupts(m), 1);
	}

	printf("== modern SLPCTRL (sim_tiny3217) ==\n");
	{
		const avr_io_addr_t S = 0x50;
		enum { CTRLA = 0x00 };
		enum { SEN = 0x01, SMODE_PDOWN = (2 << 1) };
		const uint16_t SLEEP_OP = 0x9588;

		avr_t *m = avr_make_mcu_by_name("attiny3217");
		if (!m) { printf("cannot make attiny3217 core\n"); return 2; }
		m->log = LOG_ERROR;
		avr_init(m);

		/* With SEN clear, SLEEP is a no-op on modern cores. */
		put16(m, 0, SLEEP_OP); m->pc = 0; m->state = cpu_Running;
		cpu_write(m, S + CTRLA, 0x00);
		m->pc = avr_run_one(m);
		check("no sleep when SEN clear", m->state == cpu_Sleeping, 0);

		/* SMODE is stored for read-back; SEN drives the sleep gate. */
		cpu_write(m, S + CTRLA, SEN | SMODE_PDOWN);
		check("SLPCTRL CTRLA stores SEN|SMODE", cpu_read(m, S + CTRLA), SEN | SMODE_PDOWN);

		/* With SEN set, SLEEP puts the CPU to sleep. */
		put16(m, 0, SLEEP_OP); m->pc = 0; m->state = cpu_Running;
		m->pc = avr_run_one(m);
		check("sleeps when SEN set", m->state == cpu_Sleeping, 1);

		/* Clearing SEN again disables sleep. */
		cpu_write(m, S + CTRLA, 0x00);
		put16(m, 0, SLEEP_OP); m->pc = 0; m->state = cpu_Running;
		m->pc = avr_run_one(m);
		check("no sleep after SEN cleared", m->state == cpu_Sleeping, 0);
	}

	printf("== modern RSTCTRL (sim_tiny3217) ==\n");
	{
		const avr_io_addr_t R = 0x40;
		enum { RSTFR = 0x00, SWRR = 0x01 };
		enum { PORF = 0x01, SWRF = 0x10, SWRE = 0x01 };

		avr_t *m = avr_make_mcu_by_name("attiny3217");
		if (!m) { printf("cannot make attiny3217 core\n"); return 2; }
		m->log = LOG_ERROR;
		avr_init(m);
		put16(m, 0, 0xcfff);	/* RJMP .-2 self-loop */
		m->pc = 0;

		/* Power-on reset flag is set at the initial power-up. */
		check("RSTFR.PORF set at power-on", !!(m->data[R + RSTFR] & PORF), 1);

		/* RSTFR is write-1-to-clear. */
		cpu_write(m, R + RSTFR, PORF);
		check("RSTFR.PORF cleared by W1C", !!(m->data[R + RSTFR] & PORF), 0);

		/* Run a little so the cycle counter is non-zero, then software-reset. */
		for (int i = 0; i < 10; i++)
			avr_run(m);
		long before = (long)m->cycle;
		cpu_write(m, R + SWRR, SWRE);	/* arm the software reset */
		avr_run(m);			/* performs avr_reset() */

		check("software reset zeroed the cycle counter", (long)m->cycle < before, 1);
		check("RSTFR.SWRF set after software reset", !!(m->data[R + RSTFR] & SWRF), 1);
		check("PORF not re-set on software reset", !!(m->data[R + RSTFR] & PORF), 0);
	}

	printf("== modern BOD / VLM (sim_tiny3217) ==\n");
	{
		const avr_io_addr_t B = 0x80;
		enum { VLMIE = 0x01, VLMCFG_ABOVE = 0x02 };
		enum { VLMIF = 0x01, VLMS = 0x01 };

		avr_t *m = avr_make_mcu_by_name("attiny3217");
		if (!m) { printf("cannot make attiny3217 core\n"); return 2; }
		m->log = LOG_ERROR;
		/* FUSE.BODCFG: ACTIVE=ENABLED (bits 3:2 = 0x1) and LVL=BODLEVEL2 (2.6V,
		 * bits 7:5 = 0x2) so the BOD is enabled with a known threshold. */
		m->fuse[1] = 0x44;
		avr_init(m);
		memset(m->flash, 0, 0x2000);

		/* CTRLA/CTRLB load from the fuse; the VLM registers reset to 0. */
		check("BOD CTRLA loaded from fuse (ACTIVE)", cpu_read(m, B + BODR_CTRLA), 0x04);
		check("BOD CTRLB loaded from fuse (LVL=2)", cpu_read(m, B + BODR_CTRLB), 0x02);
		check("BOD VLMCTRLA reset 0", cpu_read(m, B + BODR_VLMCTRLA), 0);
		check("BOD INTFLAGS reset 0", cpu_read(m, B + BODR_INTFLAGS), 0);

		/* CTRLB is read-only (fuse-loaded). */
		cpu_write(m, B + BODR_CTRLB, 0x05);
		check("BOD CTRLB read-only", cpu_read(m, B + BODR_CTRLB), 0x02);

		/* CTRLA: ACTIVE/SAMPFREQ read-only, only SLEEP[1:0] writable. */
		cpu_write(m, B + BODR_CTRLA, 0xff);
		check("BOD CTRLA only SLEEP writable", cpu_read(m, B + BODR_CTRLA), 0x07);

		avr_irq_t *vdd = avr_io_getirq(m, AVR_IOCTL_BOD_GETIRQ('0'), AVR_BOD_IRQ_VDD_IN);
		check("BOD VDD irq exists", vdd != NULL, 1);

		/* VLM threshold = 2.6V + 5% (VLMLVL=0) = 2730 mV. Enable a BELOW-trigger
		 * interrupt (VLMCFG=0, VLMIE). */
		cpu_write(m, B + BODR_INTCTRL, VLMIE);

		/* VDD above threshold: VLMS clear, no flag. */
		avr_raise_irq(vdd, 3300);
		check("VLMS 0 (3300 > 2730)", !!(cpu_read(m, B + BODR_STATUS) & VLMS), 0);
		check("no VLMIF above threshold", !!(cpu_read(m, B + BODR_INTFLAGS) & VLMIF), 0);

		/* VDD falls below threshold: VLMS set, VLMIF set, interrupt raised. */
		avr_raise_irq(vdd, 2500);
		check("VLMS 1 (2500 < 2730)", !!(cpu_read(m, B + BODR_STATUS) & VLMS), 1);
		check("VLMIF set on fall below", !!(cpu_read(m, B + BODR_INTFLAGS) & VLMIF), 1);
		check("BOD raises VLM interrupt", avr_has_pending_interrupts(m), 1);

		/* W1C the flag; a rise back up must NOT re-set it in BELOW mode. */
		cpu_write(m, B + BODR_INTFLAGS, VLMIF);
		check("VLMIF cleared by W1C", !!(cpu_read(m, B + BODR_INTFLAGS) & VLMIF), 0);
		avr_raise_irq(vdd, 3300);
		check("rise ignored in BELOW mode", !!(cpu_read(m, B + BODR_INTFLAGS) & VLMIF), 0);
		check("VLMS 0 after rise", !!(cpu_read(m, B + BODR_STATUS) & VLMS), 0);

		/* ABOVE mode flags the upward crossing instead. */
		cpu_write(m, B + BODR_INTCTRL, VLMIE | VLMCFG_ABOVE);
		avr_raise_irq(vdd, 2500);			/* below first (no flag) */
		check("ABOVE: no flag on fall", !!(cpu_read(m, B + BODR_INTFLAGS) & VLMIF), 0);
		avr_raise_irq(vdd, 3300);			/* rise above */
		check("ABOVE: VLMIF set on rise", !!(cpu_read(m, B + BODR_INTFLAGS) & VLMIF), 1);
		cpu_write(m, B + BODR_INTFLAGS, VLMIF);

		/* VLMLVL=2 raises the margin to +25%: threshold = 2600*1.25 = 3250 mV. */
		cpu_write(m, B + BODR_VLMCTRLA, 0x02);
		avr_raise_irq(vdd, 3300);
		check("VLMS 0 (3300 > 3250)", !!(cpu_read(m, B + BODR_STATUS) & VLMS), 0);
		avr_raise_irq(vdd, 3100);
		check("VLMS 1 (3100 < 3250)", !!(cpu_read(m, B + BODR_STATUS) & VLMS), 1);
	}

	printf("== modern BOD disabled by fuse (sim_tiny3217) ==\n");
	{
		const avr_io_addr_t B = 0x80;
		enum { VLMIE = 0x01, VLMIF = 0x01, VLMS = 0x01 };

		avr_t *m = avr_make_mcu_by_name("attiny3217");
		if (!m) { printf("cannot make attiny3217 core\n"); return 2; }
		m->log = LOG_ERROR;
		avr_init(m);			/* fuse[1] = 0 => BOD disabled (ACTIVE=DIS) */
		memset(m->flash, 0, 0x2000);

		check("BOD CTRLA disabled at reset", cpu_read(m, B + BODR_CTRLA), 0x00);
		cpu_write(m, B + BODR_INTCTRL, VLMIE);

		/* With the BOD disabled, VLMS/VLMIF are not updated even below threshold. */
		avr_irq_t *vdd = avr_io_getirq(m, AVR_IOCTL_BOD_GETIRQ('0'), AVR_BOD_IRQ_VDD_IN);
		avr_raise_irq(vdd, 1000);
		check("VLMS not updated (BOD off)", !!(cpu_read(m, B + BODR_STATUS) & VLMS), 0);
		check("VLMIF not set (BOD off)", !!(cpu_read(m, B + BODR_INTFLAGS) & VLMIF), 0);
		check("no interrupt (BOD off)", avr_has_pending_interrupts(m), 0);
	}

	printf("== modern BOD brown-out reset (sim_tiny3217) ==\n");
	{
		const avr_io_addr_t R = 0x40;
		enum { BODR_VDD = 0 };
		enum { RSTFR = 0x00 };
		enum { PORF = 0x01, BORF = 0x02 };

		avr_t *m = avr_make_mcu_by_name("attiny3217");
		if (!m) { printf("cannot make attiny3217 core\n"); return 2; }
		m->log = LOG_ERROR;
		m->fuse[1] = 0x44;	/* BOD enabled, LVL = BODLEVEL2 (2.6V) */
		avr_init(m);
		put16(m, 0, 0xcfff);	/* RJMP .-2 self-loop */
		m->pc = 0;

		/* Run a little so the cycle counter is non-zero; clear the power-on flag. */
		for (int i = 0; i < 10; i++)
			avr_run(m);
		cpu_write(m, R + RSTFR, PORF);	/* W1C the power-on flag */
		long before = (long)m->cycle;

		avr_irq_t *vdd = avr_io_getirq(m, AVR_IOCTL_BOD_GETIRQ('0'), BODR_VDD);

		/* VDD above the 2.6 V BOD level: no reset. */
		avr_raise_irq(vdd, 3000);
		avr_run(m);
		check("no brown-out above BOD level", (long)m->cycle >= before, 1);

		/* VDD falls below the BOD level: a brown-out reset is armed. */
		avr_raise_irq(vdd, 2000);
		avr_run(m);			/* performs the reset */
		check("brown-out zeroed the cycle counter", (long)m->cycle < before, 1);
		check("RSTFR.BORF set after brown-out", !!(m->data[R + RSTFR] & BORF), 1);
		check("PORF not set by brown-out", !!(m->data[R + RSTFR] & PORF), 0);
	}

	printf("== megaAVR-0 core descriptor + optional instances ==\n");
	{
		avr_t *m8 = avr_make_mcu_by_name("atmega808");
		avr_t *m9 = avr_make_mcu_by_name("atmega4809");
		if (!m8 || !m9) { printf("cannot make megaAVR-0 core\n"); return 2; }
		m8->log = LOG_ERROR;
		m9->log = LOG_ERROR;
		avr_init(m8);
		avr_init(m9);

		check("atmega808 is MODERN", !!(m8->arch.flags & AVR_ARCH_F_MODERN), 1);
		check("atmega808 vector_size 4", m8->vector_size, 4);
		check("atmega808 flashmap 0x4000", m8->arch.flashmap_start, 0x4000);
		check("atmega808 TWI0 base hooked at 0x8A0",
			  !!m8->io[AVR_DATA_TO_IO(0x08a0 + TWIM_MADDR)].w.c, 1);
		check("atmega808 has no USART3 TXDATAL hook",
			  !!m8->io[AVR_DATA_TO_IO(0x0860 + USARTR_TXDATAL)].w.c, 0);
		check("atmega4809 USART3 TXDATAL hooked",
			  !!m9->io[AVR_DATA_TO_IO(0x0860 + USARTR_TXDATAL)].w.c, 1);
		check("atmega4809 TCB3 CNTL hooked",
			  !!m9->io[AVR_DATA_TO_IO(0x0ab0 + TCBR_CNTL)].r.c, 1);

		/* 48 KB flash is not a power of two: the whole 0x4000-0xFFFF window
		 * maps 1:1, so data 0xB424 must read flash[0x7424], not a masked
		 * alias in the low half (Csmith fuzz seed 292515608). */
		m9->flash[0x7424] = 0x5c;
		m9->flash[0x3424] = 0x00;
		m9->flash[0xbfff] = 0xc3;
		put16(m9, 0, 0x9000); put16(m9, 2, 0xb424);	/* LDS r0, 0xB424 */
		put16(m9, 4, 0x9010); put16(m9, 6, 0xffff);	/* LDS r1, 0xFFFF */
		m9->pc = 0;
		m9->pc = avr_run_one(m9);
		m9->pc = avr_run_one(m9);
		check("atmega4809 LDS 0xB424 reads flash[0x7424]", m9->data[0], 0x5c);
		check("atmega4809 LDS 0xFFFF reads last flash byte", m9->data[1], 0xc3);
	}

	printf("== megaAVR-0 CCL (sim_mega4809) ==\n");
	{
		const avr_io_addr_t L = 0x1c0;
		enum { CTRLA = 0x00, L0CTRLA = 0x08, L0CTRLB = 0x09, L0CTRLC = 0x0a,
			   TRUTH0 = 0x0b, L3CTRLA = 0x14, L3CTRLB = 0x15, L3CTRLC = 0x16,
			   TRUTH3 = 0x17 };
		enum { CCL_EN = 0x01, LUT_EN = 0x01 };

		avr_t *m = avr_make_mcu_by_name("atmega4809");
		if (!m) { printf("cannot make atmega4809 core\n"); return 2; }
		m->log = LOG_ERROR;
		avr_init(m);

		uint8_t l0 = 0, l3 = 0;
		avr_irq_register_notify(
			avr_io_getirq(m, AVR_IOCTL_CCL_GETIRQ('0'), 12), rec_irq, &l0);
		avr_irq_register_notify(
			avr_io_getirq(m, AVR_IOCTL_CCL_GETIRQ('0'), 15), rec_irq, &l3);
		avr_irq_t *in0 = avr_io_getirq(m, AVR_IOCTL_CCL_GETIRQ('0'), 0);
		avr_irq_t *in1 = avr_io_getirq(m, AVR_IOCTL_CCL_GETIRQ('0'), 1);

		cpu_write(m, L + L0CTRLB, (5 << 4) | 5);
		cpu_write(m, L + L0CTRLC, 0x00);
		cpu_write(m, L + TRUTH0, 0x08);
		cpu_write(m, L + L0CTRLA, LUT_EN);
		cpu_write(m, L + CTRLA, CCL_EN);
		avr_raise_irq(in0, 1); avr_raise_irq(in1, 1);
		check("mega CCL LUT0 AND(1,1) = 1", l0, 1);

		/* On the 4-LUT megaAVR-0 CCL, LINK on LUT3 wraps to LUT0. */
		cpu_write(m, L + L3CTRLB, 0x02);
		cpu_write(m, L + L3CTRLC, 0x00);
		cpu_write(m, L + TRUTH3, 0x55);
		cpu_write(m, L + L3CTRLA, LUT_EN);
		check("mega CCL LUT3 = NOT(LUT0=1) = 0", l3, 0);
		avr_raise_irq(in0, 0);
		check("mega CCL LUT0 AND(0,1) = 0", l0, 0);
		check("mega CCL LUT3 = NOT(LUT0=0) = 1", l3, 1);

		/* Event/peripheral sources use the megaAVR-0 INSEL map, which differs
		 * from tinyAVR-1: 0xA = TCA0, 0xC = TCB, 0x8 = USART TXD. */
		avr_irq_t *s_tca0 = avr_io_getirq(m, AVR_IOCTL_CCL_GETIRQ('0'),
			AVR_CCL_IRQ_SRC_4LUT(AVR_CCL_SRC_TCA0_WO0));
		avr_irq_t *s_tcb1 = avr_io_getirq(m, AVR_IOCTL_CCL_GETIRQ('0'),
			AVR_CCL_IRQ_SRC_4LUT(AVR_CCL_SRC_TCB1));
		avr_irq_t *s_us0 = avr_io_getirq(m, AVR_IOCTL_CCL_GETIRQ('0'),
			AVR_CCL_IRQ_SRC_4LUT(AVR_CCL_SRC_USART0_TXD));

		cpu_write(m, L + L3CTRLA, 0x00);	/* drop the LINK LUT */
		cpu_write(m, L + L0CTRLC, 0x00);
		cpu_write(m, L + TRUTH0, 0xaa);		/* OUT = IN0 */

		cpu_write(m, L + CTRLA, 0x00);
		cpu_write(m, L + L0CTRLB, 0x0a);	/* IN0 = TCA0 WO0 (mega 0xA) */
		cpu_write(m, L + CTRLA, CCL_EN);
		avr_raise_irq(s_tca0, 1);
		check("mega CCL IN0=TCA0(0xA) follows WO0", l0, 1);
		avr_raise_irq(s_tca0, 0);
		check("mega CCL IN0=TCA0 WO0 low", l0, 0);

		cpu_write(m, L + CTRLA, 0x00);
		cpu_write(m, L + L0CTRLB, 0x08);	/* IN0 = USART0 TXD (mega 0x8) */
		cpu_write(m, L + CTRLA, CCL_EN);
		avr_raise_irq(s_us0, 1);
		check("mega CCL IN0=USART0(0x8) follows TXD", l0, 1);
		avr_raise_irq(s_us0, 0);

		/* Position decode: INSEL=0xC on IN1 selects TCB1 WO. */
		cpu_write(m, L + CTRLA, 0x00);
		cpu_write(m, L + L0CTRLB, (0x0c << 4) | 0x00);	/* IN1=TCB(0xC), IN0=MASK */
		cpu_write(m, L + TRUTH0, 0xcc);			/* OUT = IN1 */
		cpu_write(m, L + CTRLA, CCL_EN);
		avr_raise_irq(s_tcb1, 1);
		check("mega CCL IN1=TCB(0xC) selects TCB1 (position decode)", l0, 1);
		avr_raise_irq(s_tcb1, 0);
		check("mega CCL IN1=TCB1 low", l0, 0);

		/* End-to-end: AC0 OUT is wired to the CCL AC0 source by the core. */
		cpu_write(m, L + CTRLA, 0x00);
		cpu_write(m, L + L0CTRLB, 0x06);	/* IN0 = AC0 (mega 0x6) */
		cpu_write(m, L + L0CTRLC, 0x00);
		cpu_write(m, L + TRUTH0, 0xaa);		/* OUT = IN0 */
		cpu_write(m, L + CTRLA, CCL_EN);
		avr_raise_irq(avr_io_getirq(m, AVR_IOCTL_AC_GETIRQ('0'), AVR_AC_IRQ_AINP0), 800);
		avr_raise_irq(avr_io_getirq(m, AVR_IOCTL_AC_GETIRQ('0'), AVR_AC_IRQ_AINN0), 500);
		cpu_write(m, 0x680 + ACR_CTRLA, 0x01);	/* AC enable; V+ 800 > V- 500 */
		check("mega CCL LUT0 follows AC0 high (real comparator)", l0, 1);
		avr_raise_irq(avr_io_getirq(m, AVR_IOCTL_AC_GETIRQ('0'), AVR_AC_IRQ_AINN0), 900);
		check("mega CCL LUT0 follows AC0 low (real comparator)", l0, 0);
	}

	printf("== megaAVR-0 EVSYS + ADC event start (sim_mega4809) ==\n");
	{
		const avr_io_addr_t E = 0x180, C = 0x680, A = 0x600;
		enum { STROBE = 0x00, CHANNEL0 = 0x10, USERADC0 = 0x28 };
		enum { AC_ENABLE = 0x01, MUXNEG_VREF = 0x02 };
		enum { GEN_AC0_OUT = 0x20, SEL_CH0 = 1 };
		enum { ADC_ENABLE = 0x01, STARTEI = 0x01 };
		enum { F_RESRDY = 0x01 };

		avr_t *m = avr_make_mcu_by_name("atmega4809");
		if (!m) { printf("cannot make atmega4809 core\n"); return 2; }
		m->log = LOG_ERROR;
		avr_init(m);

		uint8_t uadc = 0;
		avr_irq_register_notify(
			avr_io_getirq(m, AVR_IOCTL_EVSYS_GETIRQ('0'), AVR_EVSYS_IRQ_USER0 + 8),
			rec_irq, &uadc);

		avr_irq_t *ainp0 = avr_io_getirq(m, AVR_IOCTL_AC_GETIRQ('0'), AVR_AC_IRQ_AINP0);
		avr_raise_irq(ainp0, 500);
		cpu_write(m, C + ACR_MUXCTRLA, MUXNEG_VREF);
		cpu_write(m, C + ACR_CTRLA, AC_ENABLE);

		cpu_write(m, E + CHANNEL0, GEN_AC0_OUT);
		cpu_write(m, E + USERADC0, SEL_CH0);

		avr_raise_irq(avr_io_getirq(m, AVR_IOCTL_ADCM_GETIRQ('0'), 0), 1650);
		cpu_write(m, A + ADCMR_MUXPOS, 0);
		cpu_write(m, A + ADCMR_EVCTRL, STARTEI);
		cpu_write(m, A + ADCMR_CTRLA, ADC_ENABLE);
		cpu_write(m, A + ADCMR_INTFLAGS, F_RESRDY);

		avr_raise_irq(ainp0, 2000);
		for (int i = 0; i < 400 && !(m->data[A + ADCMR_INTFLAGS] & F_RESRDY); i++)
			avr_run(m);
		check("mega EVSYS USERADC0 saw AC0 event", uadc, 1);
		check("mega EVSYS started ADC conversion",
			  !!(m->data[A + ADCMR_INTFLAGS] & F_RESRDY), 1);
		check("mega event-triggered result = 512",
			  (cpu_read(m, A + ADCMR_RESL) | (cpu_read(m, A + ADCMR_RESH) << 8)), 512);

		g_evsys_pulses = 0;
		avr_irq_register_notify(
			avr_io_getirq(m, AVR_IOCTL_EVSYS_GETIRQ('0'), AVR_EVSYS_IRQ_USER0 + 8),
			evsys_count_hook, NULL);
		cpu_write(m, E + STROBE, 0x01);
		check("mega EVSYS strobe pulses channel 0 user once", g_evsys_pulses, 1);
	}

	printf("== modern SYSCFG / SIGROW device identity (sim_tiny3217) ==\n");
	{
		const avr_io_addr_t SYS = 0xf00, SIG = 0x1100;

		avr_t *m = avr_make_mcu_by_name("attiny3217");
		if (!m) { printf("cannot make attiny3217 core\n"); return 2; }
		m->log = LOG_ERROR;
		avr_init(m);

		/* SIGROW.DEVICEID[2:0] is the ATtiny3217 signature 0x1E 0x95 0x22. */
		check("DEVICEID0 = signature[0]", cpu_read(m, SIG + SIGROWR_DEVICEID0), 0x1e);
		check("DEVICEID1 = signature[1]", cpu_read(m, SIG + SIGROWR_DEVICEID1), 0x95);
		check("DEVICEID2 = signature[2]", cpu_read(m, SIG + SIGROWR_DEVICEID2), 0x22);

		/* DEVICEID matches the core's own signature[] table. */
		check("DEVICEID0 matches core signature", cpu_read(m, SIG + SIGROWR_DEVICEID0),
				m->signature[0]);

		/* DEVICEID is read-only: a write is ignored. */
		cpu_write(m, SIG + SIGROWR_DEVICEID0, 0x00);
		check("DEVICEID0 read-only", cpu_read(m, SIG + SIGROWR_DEVICEID0), 0x1e);

		/* SYSCFG.REVID reports revision A (0x00) and is read-only. */
		check("SYSCFG REVID = 0 (rev A)", cpu_read(m, SYS + SYSCFGR_REVID), 0x00);
		cpu_write(m, SYS + SYSCFGR_REVID, 0x07);
		check("SYSCFG REVID read-only", cpu_read(m, SYS + SYSCFGR_REVID), 0x00);

		/* SYSCFG.EXTBRK is a writable store. */
		cpu_write(m, SYS + SYSCFGR_EXTBRK, 0x01);
		check("SYSCFG EXTBRK stores", cpu_read(m, SYS + SYSCFGR_EXTBRK), 0x01);

		/* FUSE read-back window (0x1280): the CPU can read the fuses (DS40002205A
		 * 6.10). FUSE_t bytes: 1=BODCFG, 2=OSCCFG, 5=SYSCFG0, 8=BOOTEND. */
		const avr_io_addr_t FUSE = 0x1280;
		m->fuse[1] = 0x44; m->fuse[2] = 0x7e; m->fuse[5] = 0xf6; m->fuse[8] = 4;
		avr_reset(m);
		check("FUSE.BODCFG read-back", cpu_read(m, FUSE + 1), 0x44);
		check("FUSE.OSCCFG read-back", cpu_read(m, FUSE + 2), 0x7e);
		check("FUSE.SYSCFG0 read-back", cpu_read(m, FUSE + 5), 0xf6);
		check("FUSE.BOOTEND read-back", cpu_read(m, FUSE + 8), 4);
		/* The window is read-only: a direct CPU write is ignored. */
		cpu_write(m, FUSE + 2, 0x00);
		check("FUSE window read-only (OSCCFG unchanged)", cpu_read(m, FUSE + 2), 0x7e);
		/* The read is live: an NVMCTRL FUSEWRITE (modelled as an avr->fuse[]
		 * update) is reflected immediately. */
		m->fuse[2] = 0x12;
		check("FUSE window tracks fuse[] (live)", cpu_read(m, FUSE + 2), 0x12);

		/* FUSE.LOCKBIT (0x128A) reads the unlocked key 0xC5 (DS40002205A
		 * 6.10.4.9); it is read-only to the CPU. */
		check("FUSE.LOCKBIT reads unlocked key 0xC5", cpu_read(m, 0x128a), 0xc5);
		cpu_write(m, 0x128a, 0x00);
		check("FUSE.LOCKBIT read-only", cpu_read(m, 0x128a), 0xc5);

		/* SIGROW.SERNUM[0..9] is populated with a deterministic non-zero serial
		 * (real silicon is never all-zero); OSCnnERR read 0 = no error. */
		check("SIGROW.SERNUM0 populated (0x10)", cpu_read(m, SIG + 0x03), 0x10);
		check("SIGROW.SERNUM9 populated (0x19)", cpu_read(m, SIG + 0x0c), 0x19);
		int sernum_nonzero = 0;
		for (int i = 0; i < 10; i++)
			sernum_nonzero |= cpu_read(m, SIG + 0x03 + i);
		check("SIGROW.SERNUM not all-zero", !!sernum_nonzero, 1);
		check("SIGROW.OSC20ERR3V = 0 (no error)", cpu_read(m, SIG + 0x24), 0x00);
	}

	printf("== tinyAVR 0-series cores + per-device AC0/DAC0 bases ==\n");
	{
		/* Address -> "has an IO write handler" probe. */
#define HAS_W(m, a) (!!(m)->io[AVR_DATA_TO_IO(a)].w.c)

		/* ATtiny202 (DS40002318A): AC0 at 0x0670; no DAC0, no TCD0. */
		avr_t *m = avr_make_mcu_by_name("attiny202");
		if (!m) { printf("cannot make attiny202 core\n"); return 2; }
		m->log = LOG_ERROR;
		avr_init(m);
		check("attiny202 is MODERN", !!(m->arch.flags & AVR_ARCH_F_MODERN), 1);
		check("attiny202 signature 1E 91 23",
			  (m->signature[0] << 16) | (m->signature[1] << 8) | m->signature[2], 0x1e9123);
		check("attiny202 vector_size 2 (2 KB flash)", m->vector_size, 2);
		check("attiny202 AC0 handler at 0x0670", HAS_W(m, 0x0670 + ACR_MUXCTRLA), 1);
		check("attiny202 nothing at 0x0680 (no DAC0)", HAS_W(m, 0x0680), 0);
		check("attiny202 no TCD0 at 0x0A80", HAS_W(m, 0x0a80), 0);
		check("attiny202 TCB0 present", HAS_W(m, 0x0a40), 1);
		check("attiny202 no TCB1", HAS_W(m, 0x0a50), 0);

		/* ATtiny414 (DS40002287A): AC0 0x0670, DAC0 0x0680 -- the 2/4/8 KB
		 * 1-series parts, previously mapped at the 16/32 KB addresses. */
		m = avr_make_mcu_by_name("attiny414");
		if (!m) { printf("cannot make attiny414 core\n"); return 2; }
		m->log = LOG_ERROR;
		avr_init(m);
		check("attiny414 AC0 handler at 0x0670", HAS_W(m, 0x0670 + ACR_MUXCTRLA), 1);
		check("attiny414 DAC0 handler at 0x0680", HAS_W(m, 0x0680), 1);
		check("attiny414 nothing at 0x0682 (no AC0 shadow)", HAS_W(m, 0x0680 + ACR_MUXCTRLA), 0);
		check("attiny414 TCD0 present", HAS_W(m, 0x0a80), 1);

		/* ATtiny3217 keeps AC0 0x0680 / DAC0 0x06A0. */
		m = avr_make_mcu_by_name("attiny3217");
		if (!m) { printf("cannot make attiny3217 core\n"); return 2; }
		m->log = LOG_ERROR;
		avr_init(m);
		check("attiny3217 AC0 handler at 0x0680", HAS_W(m, 0x0680 + ACR_MUXCTRLA), 1);
		check("attiny3217 DAC0 handler at 0x06A0", HAS_W(m, 0x06a0), 1);

		/* ATtiny1607 (DS40002312A): AC0 0x0680, 16 KB, three ports, no DAC/TCD/
		 * TCB1; VREF.CTRLA.DAC0REFSEL is the AC0 reference. */
		m = avr_make_mcu_by_name("attiny1607");
		if (!m) { printf("cannot make attiny1607 core\n"); return 2; }
		m->log = LOG_ERROR;
		avr_init(m);
		check("attiny1607 signature 1E 94 23",
			  (m->signature[0] << 16) | (m->signature[1] << 8) | m->signature[2], 0x1e9423);
		check("attiny1607 vector_size 4 (16 KB flash)", m->vector_size, 4);
		check("attiny1607 AC0 handler at 0x0680", HAS_W(m, 0x0680 + ACR_MUXCTRLA), 1);
		check("attiny1607 no DAC0 at 0x06A0", HAS_W(m, 0x06a0), 0);
		check("attiny1607 no TCD0 at 0x0A80", HAS_W(m, 0x0a80), 0);
		check("attiny1607 PORTC present", HAS_W(m, 0x0440 + 0x01), 1);

		const avr_io_addr_t C = 0x680, V = 0x0a0;
		avr_irq_t *ainp0 = avr_io_getirq(m, AVR_IOCTL_AC_GETIRQ('0'), AVR_AC_IRQ_AINP0);
		cpu_write(m, V + VREFR_CTRLA, 0x02);		/* DAC0REFSEL = 2.5 V */
		cpu_write(m, C + ACR_MUXCTRLA, 0x02);		/* MUXNEG = VREF */
		cpu_write(m, C + ACR_CTRLA, 0x01);
		avr_raise_irq(ainp0, 2000);
		check("1607 AC0 vs VREF 2.5V: 2000 mV is below", !!(cpu_read(m, C + ACR_STATUS) & 0x10), 0);
		avr_raise_irq(ainp0, 3000);
		check("1607 AC0 vs VREF 2.5V: 3000 mV is above", !!(cpu_read(m, C + ACR_STATUS) & 0x10), 1);
#undef HAS_W
	}


	printf("== tinyAVR 2-series cores (DS40002311B / DS40002234B / DS40002345B) ==\n");
	{
		/* Address -> "has an IO write handler" probe. */
#define HAS_W(m, a) (!!(m)->io[AVR_DATA_TO_IO(a)].w.c)
		/* Datasheet Table 1 (memories), Table 7-6 (device IDs), Table 2 (ports). */
		static const struct {
			const char *name;
			uint32_t sig;
			uint16_t flash, sram, eeprom;	/* bytes */
			int portc;
		} dev[] = {
			{ "attiny424",  0x1e922c,  4096,  512, 128, 0 },
			{ "attiny426",  0x1e922b,  4096,  512, 128, 1 },
			{ "attiny427",  0x1e922a,  4096,  512, 128, 1 },
			{ "attiny824",  0x1e9329,  8192, 1024, 128, 0 },
			{ "attiny826",  0x1e9328,  8192, 1024, 128, 1 },
			{ "attiny827",  0x1e9327,  8192, 1024, 128, 1 },
			{ "attiny1624", 0x1e942a, 16384, 2048, 256, 0 },
			{ "attiny1626", 0x1e9429, 16384, 2048, 256, 1 },
			{ "attiny1627", 0x1e9428, 16384, 2048, 256, 1 },
			{ "attiny3224", 0x1e9528, 32768, 3072, 256, 0 },
			{ "attiny3226", 0x1e9527, 32768, 3072, 256, 1 },
			{ "attiny3227", 0x1e9526, 32768, 3072, 256, 1 },
		};
		for (unsigned i = 0; i < sizeof(dev) / sizeof(dev[0]); i++) {
			char what[64];
			avr_t *m = avr_make_mcu_by_name(dev[i].name);
			if (!m) { printf("cannot make %s core\n", dev[i].name); return 2; }
			m->log = LOG_ERROR;
			avr_init(m);
#define W(fmt) (snprintf(what, sizeof(what), "%s " fmt, dev[i].name), what)
			check(W("is MODERN"), !!(m->arch.flags & AVR_ARCH_F_MODERN), 1);
			check(W("signature"),
				  (m->signature[0] << 16) | (m->signature[1] << 8) | m->signature[2],
				  dev[i].sig);
			check(W("flashend"), m->flashend, dev[i].flash - 1);
			check(W("ramend 0x3FFF"), m->ramend, 0x3fff);
			check(W("e2end"), m->e2end, 0x1400 + dev[i].eeprom - 1);
			check(W("vector_size"), m->vector_size, dev[i].flash > 8192 ? 4 : 2);
			check(W("flashmap 0x8000"), m->arch.flashmap_start, 0x8000);
			check(W("SRAM start"), m->ramend - dev[i].sram + 1,
				  0x4000 - dev[i].sram);
			check(W("PORTA"), HAS_W(m, 0x0400 + 0x01), 1);
			check(W("PORTB"), HAS_W(m, 0x0420 + 0x01), 1);
			check(W("PORTC fitted"), HAS_W(m, 0x0440 + 0x01), dev[i].portc);
			check(W("USART0 @0x800"), HAS_W(m, 0x0802), 1);
			check(W("USART1 @0x820"), HAS_W(m, 0x0822), 1);
			check(W("TWI0 @0x8A0"), HAS_W(m, 0x08a4), 1);
			check(W("SPI0 @0x8C0"), HAS_W(m, 0x08c1), 1);
			check(W("TCA0 @0xA00"), HAS_W(m, 0x0a00), 1);
			check(W("TCB0 @0xA80"), HAS_W(m, 0x0a80), 1);
			check(W("TCB1 @0xA90"), HAS_W(m, 0x0a90), 1);
			check(W("PORTMUX @0x5E0"), HAS_W(m, 0x05e0), 1);
			check(W("ADC0 @0x600"), HAS_W(m, 0x060a), 1);
			check(W("AC0 @0x680"), HAS_W(m, 0x0680), 1);
			check(W("no DAC0 @0x6A0"), HAS_W(m, 0x06a0), 0);
			check(W("no TCB0 @1-series 0xA40"), HAS_W(m, 0x0a40), 0);
			check(W("no TWI0 @1-series 0x810"), HAS_W(m, 0x0814), 0);
		}
#undef W
	}

	printf("== tinyAVR 2-series PORTMUX (six *ROUTEA registers) ==\n");
	{
		avr_t *m = avr_make_mcu_by_name("attiny3226");
		if (!m) { printf("cannot make attiny3226 core\n"); return 2; }
		m->log = LOG_ERROR;
		avr_init(m);
		const avr_io_addr_t P = 0x05e0;
		for (int i = 0; i < 6; i++)
			cpu_write(m, P + i, 0x10 + i);
		for (int i = 0; i < 6; i++) {
			char w[32]; snprintf(w, sizeof(w), "PORTMUX reg %d stores", i);
			check(w, cpu_read(m, P + i), 0x10 + i);
		}
		check("PORTMUX has no 7th register", HAS_W(m, P + 6), 0);
		avr_reset(m);
		check("PORTMUX reset clears EVSYSROUTEA", m->data[P], 0);
		check("PORTMUX reset clears TCBROUTEA", m->data[P + 5], 0);
	}

	printf("== megaAVR-0 PORTMUX (six *ROUTEA registers) ==\n");
	{
		avr_t *m = avr_make_mcu_by_name("atmega4809");
		if (!m) { printf("cannot make atmega4809 core\n"); return 2; }
		m->log = LOG_ERROR;
		avr_init(m);
		const avr_io_addr_t P = 0x05e0;
		for (int i = 0; i < 6; i++)
			cpu_write(m, P + i, 0x10 + i);
		for (int i = 0; i < 6; i++) {
			char w[40]; snprintf(w, sizeof(w), "4809 PORTMUX reg %d stores", i);
			check(w, cpu_read(m, P + i), 0x10 + i);
		}
		check("4809 PORTMUX TCAROUTEA hooked", HAS_W(m, P + 4), 1);
		check("4809 PORTMUX TCBROUTEA hooked", HAS_W(m, P + 5), 1);
		avr_reset(m);
		check("4809 PORTMUX reset clears TCAROUTEA", m->data[P + 4], 0);
		check("4809 PORTMUX reset clears TCBROUTEA", m->data[P + 5], 0);
	}

	printf("== tinyAVR 2-series VREF + AC0 (AC0REFSEL, DACREF, AINN2, hysteresis) ==\n");
	{
		avr_t *m = avr_make_mcu_by_name("attiny3226");
		if (!m) { printf("cannot make attiny3226 core\n"); return 2; }
		m->log = LOG_ERROR;
		avr_init(m);
		const avr_io_addr_t C = 0x680, V = 0x0a0;
		avr_irq_t *ainp0 = avr_io_getirq(m, AVR_IOCTL_AC_GETIRQ('0'), AVR_AC_IRQ_AINP0);
		avr_irq_t *ainn0 = avr_io_getirq(m, AVR_IOCTL_AC_GETIRQ('0'), AVR_AC_IRQ_AINN0);
		avr_irq_t *ainn2 = avr_io_getirq(m, AVR_IOCTL_AC_GETIRQ('0'), AVR_AC_IRQ_AINN2);
		check("AC AINN2 irq exists", ainn2 != NULL, 1);
		check("DACREF resets to 0xFF", m->data[C + ACR_DACREF], 0xff);

		/* MUXNEG = DACREF (3); VREF.AC0REFSEL = 1.024 V at reset: 0xFF -> 1020 mV. */
		cpu_write(m, C + ACR_MUXCTRLA, 0x03);
		cpu_write(m, C + ACR_CTRLA, 0x01);
		avr_raise_irq(ainp0, 1000);
		check("DACREF 0xFF @1.024V = 1020mV: 1000 below", !!(cpu_read(m, C + ACR_STATUS) & 0x10), 0);
		avr_raise_irq(ainp0, 1040);
		check("DACREF 0xFF @1.024V = 1020mV: 1040 above", !!(cpu_read(m, C + ACR_STATUS) & 0x10), 1);

		/* AC0REFSEL = 2.5 V (2), DACREF = 0x80 -> 1250 mV. */
		cpu_write(m, V + VREFR_CTRLA, 0x02);
		cpu_write(m, C + ACR_DACREF, 0x80);
		avr_raise_irq(ainp0, 1200);
		check("DACREF 0x80 @2.5V = 1250mV: 1200 below", !!(cpu_read(m, C + ACR_STATUS) & 0x10), 0);
		avr_raise_irq(ainp0, 1300);
		check("DACREF 0x80 @2.5V = 1250mV: 1300 above", !!(cpu_read(m, C + ACR_STATUS) & 0x10), 1);

		/* The reference tracks AC0REFSEL: 4.096 V (3) -> DACREF 0x80 = 2048 mV. */
		cpu_write(m, V + VREFR_CTRLA, 0x03);
		check("AC0REFSEL 4.096V: 1300 now below", !!(cpu_read(m, C + ACR_STATUS) & 0x10), 0);
		/* AC0REFSEL = VDD (7) -> 3300 mV * 128/256 = 1650 mV. */
		cpu_write(m, V + VREFR_CTRLA, 0x07);
		avr_raise_irq(ainp0, 1700);
		check("AC0REFSEL VDD: DACREF 0x80 = 1650mV, 1700 above", !!(cpu_read(m, C + ACR_STATUS) & 0x10), 1);
		cpu_write(m, V + VREFR_CTRLA, 0xff);	/* only [2:0] are implemented */
		check("VREF.CTRLA keeps only AC0REFSEL[2:0]", m->data[V + VREFR_CTRLA], 0x07);
		cpu_write(m, V + VREFR_CTRLA, 0x05);	/* reserved selection -> 0 mV ref */
		avr_raise_irq(ainp0, 10);
		check("reserved AC0REFSEL: DACREF output is 0", !!(cpu_read(m, C + ACR_STATUS) & 0x10), 1);

		/* MUXNEG = AINN2 (2) is a real pin on the 2-series (not VREF). */
		cpu_write(m, C + ACR_MUXCTRLA, 0x02);
		avr_raise_irq(ainn2, 1500);
		avr_raise_irq(ainp0, 1400);
		check("MUXNEG AINN2: 1400 < 1500", !!(cpu_read(m, C + ACR_STATUS) & 0x10), 0);
		avr_raise_irq(ainp0, 1600);
		check("MUXNEG AINN2: 1600 > 1500", !!(cpu_read(m, C + ACR_STATUS) & 0x10), 1);

		/* HYSMODE = LARGE (3): VHYS 55 mV typ, modelled (assumption) as a +/-28 mV
		 * half-window around the crossing. */
		cpu_write(m, C + ACR_MUXCTRLA, 0x00);	/* AINP0 vs AINN0 */
		avr_raise_irq(ainn0, 1000);
		avr_raise_irq(ainp0, 900);
		cpu_write(m, C + ACR_CTRLA, 0x01 | (3 << 1));
		avr_raise_irq(ainp0, 1010);
		check("hysteresis: +10 mV is inside the band, stays low", !!(cpu_read(m, C + ACR_STATUS) & 0x10), 0);
		avr_raise_irq(ainp0, 1040);
		check("hysteresis: +40 mV trips high", !!(cpu_read(m, C + ACR_STATUS) & 0x10), 1);
		avr_raise_irq(ainp0, 990);
		check("hysteresis: -10 mV holds high", !!(cpu_read(m, C + ACR_STATUS) & 0x10), 1);
		avr_raise_irq(ainp0, 960);
		check("hysteresis: -40 mV trips low", !!(cpu_read(m, C + ACR_STATUS) & 0x10), 0);

		/* Reset restores DACREF=0xFF and AC0REFSEL=1.024 V. */
		cpu_write(m, C + ACR_DACREF, 0x10);
		avr_reset(m);
		check("reset: DACREF back to 0xFF", m->data[C + ACR_DACREF], 0xff);
		check("reset: VREF.CTRLA back to 0", m->data[V + VREFR_CTRLA], 0);
	}

	printf("== tinyAVR 2-series EVSYS (6 channels, 20 users) + CCL interrupt ==\n");
	{
		avr_t *m = avr_make_mcu_by_name("attiny3226");
		if (!m) { printf("cannot make attiny3226 core\n"); return 2; }
		m->log = LOG_ERROR;
		avr_init(m);
		memset(m->flash, 0, 0x2000);
		const avr_io_addr_t E = 0x180, C = 0x1c0;
		check("EVSYS user 19 (USERTCB1COUNT) handled", HAS_W(m, E + 0x20 + 19), 1);
		check("EVSYS has no user 20", HAS_W(m, E + 0x20 + 20), 0);

		/* SWEVENTA bit n strobes channel n (0..5); users routed to it see a pulse. */
		g_evsys_pulses = 0;
		avr_irq_register_notify(avr_io_getirq(m, AVR_IOCTL_EVSYS_GETIRQ('0'),
							AVR_EVSYS_IRQ_USER0 + 8), evsys_count_hook, NULL);
		cpu_write(m, E + 0x28, 6);			/* USERADC0START -> CHANNEL5 (value 6) */
		cpu_write(m, E + 0x00, 1 << 5);
		check("SWEVENTA bit 5 pulses CHANNEL5's user", g_evsys_pulses, 1);
		cpu_write(m, E + 0x00, 1 << 0);
		check("SWEVENTA bit 0 does not reach a CHANNEL5 user", g_evsys_pulses, 1);
		cpu_write(m, E + 0x28, 7);			/* value 7 would be CHANNEL6: not a channel */
		cpu_write(m, E + 0x00, 1 << 6);
		check("no sixth-index channel (CHANNEL6)", g_evsys_pulses, 1);

		/* CCL: LUT0 follows IN0 (INSEL0=IO, TRUTH 0xAA); INTMODEn selects the edge. */
		avr_irq_t *in0 = avr_io_getirq(m, AVR_IOCTL_CCL_GETIRQ('0'), AVR_CCL_IRQ_LUT0_IN0);
		cpu_write(m, C + 0x09, 0x05);			/* LUT0CTRLB: INSEL0 = IO, INSEL1 = MASK */
		cpu_write(m, C + 0x0a, 0x00);			/* LUT0CTRLC: INSEL2 = MASK */
		cpu_write(m, C + 0x0b, 0xaa);			/* TRUTH0: out = IN0 */
		cpu_write(m, C + 0x08, 0x01);			/* LUT0CTRLA: ENABLE */
		cpu_write(m, C + 0x00, 0x01);			/* CTRLA: ENABLE */
		cpu_write(m, C + 0x05, 0x01);			/* INTMODE0 = RISING */
		avr_raise_irq(in0, 1);
		check("CCL rising edge sets INT0", m->data[C + 0x07] & 0x01, 1);
		check("CCL interrupt dispatches vector 5", take_vector(m), 5);
		avr_raise_irq(in0, 0);
		check("RISING mode: falling edge sets no extra flag", m->data[C + 0x07], 0x01);
		cpu_write(m, C + 0x07, 0x01);			/* W1C */
		check("INTFLAGS W1C clears INT0", m->data[C + 0x07], 0);
		check("request dropped with the flag", take_vector(m), -1);

		cpu_write(m, C + 0x05, 0x02);			/* FALLING */
		avr_raise_irq(in0, 1);
		check("FALLING: rising edge sets nothing", m->data[C + 0x07], 0);
		avr_raise_irq(in0, 0);
		check("FALLING: falling edge sets INT0", m->data[C + 0x07] & 0x01, 1);
		check("FALLING dispatches vector 5", take_vector(m), 5);
		cpu_write(m, C + 0x07, 0x01);

		cpu_write(m, C + 0x05, 0x03);			/* BOTH */
		avr_raise_irq(in0, 1);
		check("BOTH: rising edge sets INT0", m->data[C + 0x07] & 0x01, 1);
		cpu_write(m, C + 0x07, 0x01);
		avr_raise_irq(in0, 0);
		check("BOTH: falling edge sets INT0", m->data[C + 0x07] & 0x01, 1);
		cpu_write(m, C + 0x07, 0x01);

		cpu_write(m, C + 0x05, 0x00);			/* INTDISABLE */
		avr_raise_irq(in0, 1);
		check("INTDISABLE: no flag", m->data[C + 0x07], 0);
		avr_raise_irq(in0, 0);

		/* LUT2 uses INTMODE2 (bits 5:4) and INT2; the four LUTs share vector 5. */
		avr_irq_t *l2in0 = avr_io_getirq(m, AVR_IOCTL_CCL_GETIRQ('0'), AVR_CCL_IRQ_LUT0_IN0 + 6);
		cpu_write(m, C + 0x11, 0x05);			/* LUT2CTRLB */
		cpu_write(m, C + 0x12, 0x00);
		cpu_write(m, C + 0x13, 0xaa);			/* TRUTH2 */
		cpu_write(m, C + 0x10, 0x01);			/* LUT2CTRLA: ENABLE */
		cpu_write(m, C + 0x05, 1 << 4);			/* INTMODE2 = RISING */
		avr_raise_irq(l2in0, 1);
		check("LUT2 rising edge sets INT2 only", m->data[C + 0x07], 0x04);
		check("LUT2 dispatches the shared vector 5", take_vector(m), 5);
		cpu_write(m, C + 0x07, 0x04);

		/* Enabling an edge mode while a flag is pending re-requests (level IRQ). */
		cpu_write(m, C + 0x05, 0x01);
		avr_raise_irq(avr_io_getirq(m, AVR_IOCTL_CCL_GETIRQ('0'), AVR_CCL_IRQ_LUT0_IN0), 0);
		avr_raise_irq(avr_io_getirq(m, AVR_IOCTL_CCL_GETIRQ('0'), AVR_CCL_IRQ_LUT0_IN0), 1);
		take_vector(m);					/* service it; flag stays sticky */
		cpu_write(m, C + 0x05, 0x00);
		cpu_write(m, C + 0x05, 0x01);
		check("re-enabling INTMODE with INT0 still set re-requests", take_vector(m), 5);
		cpu_write(m, C + 0x07, 0x0f);

		/* LUT output also drives EVSYS generator 0x10 (+ LUT n). */
		g_evsys_pulses = 0;
		cpu_write(m, E + 0x13, 0x10);			/* CHANNEL3 = CCL LUT0 */
		cpu_write(m, E + 0x20 + 3, 4);			/* USERCCLLUT1B -> CHANNEL3 */
		avr_irq_register_notify(avr_io_getirq(m, AVR_IOCTL_EVSYS_GETIRQ('0'),
							AVR_EVSYS_IRQ_USER0 + 3), evsys_count_hook, NULL);
		avr_raise_irq(avr_io_getirq(m, AVR_IOCTL_CCL_GETIRQ('0'), AVR_CCL_IRQ_LUT0_IN0), 0);
		avr_raise_irq(avr_io_getirq(m, AVR_IOCTL_CCL_GETIRQ('0'), AVR_CCL_IRQ_LUT0_IN0), 1);
		check("LUT0 rising edge reaches EVSYS CHANNEL3 user", g_evsys_pulses, 1);
	}

	printf("== tinyAVR 2-series ADC0: Single mode, formats, timing ==\n");
	{
		enum { CTRLA = 0, CTRLB = 1, CTRLC = 2, CTRLD = 3, INTCTRL = 4, INTFLAGS = 5,
			   STATUS = 6, CTRLE = 8, CTRLF = 9, COMMAND = 0x0a, PGACTRL = 0x0b,
			   MUXPOS = 0x0c, MUXNEG = 0x0d, RESULT = 0x10, SAMPLE = 0x14,
			   WINLT = 0x1c, WINHT = 0x1e };
		enum { F_RES = 0x01, F_SAMP = 0x02, F_WCMP = 0x04, F_RESOVR = 0x08,
			   F_SAMPOVR = 0x10, F_TRIGOVR = 0x20 };
		const avr_io_addr_t A = 0x600;
#define RES32(m) ((int32_t)((m)->data[A + RESULT] | ((m)->data[A + RESULT + 1] << 8) | \
			((m)->data[A + RESULT + 2] << 16) | ((uint32_t)(m)->data[A + RESULT + 3] << 24)))
#define SAMP16(m) ((m)->data[A + SAMPLE] | ((m)->data[A + SAMPLE + 1] << 8))
#define ADC_CLEAR(m) cpu_write((m), A + INTFLAGS, 0x3f)

		avr_t *m = avr_make_mcu_by_name("attiny3226");
		if (!m) { printf("cannot make attiny3226 core\n"); return 2; }
		m->log = LOG_ERROR;
		avr_init(m);
		memset(m->flash, 0, 0x2000);
		check("CPU clock = OSC20M/6", m->frequency, 3333333);
		check("PGACTRL resets to 0x04", m->data[A + PGACTRL], 0x04);

		avr_irq_t *ain3 = avr_io_getirq(m, AVR_IOCTL_ADC2_GETIRQ('0'), 3);
		avr_irq_t *ain5 = avr_io_getirq(m, AVR_IOCTL_ADC2_GETIRQ('0'), 5);
		check("ADC AIN3 irq exists", ain3 != NULL, 1);
		avr_raise_irq(ain3, 1650);		/* half of the 3300 mV default VDD */

		/* Single 12-bit: 1650/3300*4096 = 2048. 10 us init (33 cyc @3.33 MHz)
		 * + (SAMPDUR+15) ADC clocks at DIV2 = 30 cycles. */
		cpu_write(m, A + MUXPOS, 3);
		cpu_write(m, A + CTRLA, 0x01);
		cpu_write(m, A + COMMAND, (1 << 4) | 1);
		check("ADCBUSY set while converting", m->data[A + STATUS], 1);
		long t = run_until_set(m, A + INTFLAGS, F_RES, 400);
		check("12-bit conversion time = 10us init + 30 cyc", t >= 62 && t <= 65, 1);
		check("12-bit result = 2048", RES32(m), 2048);
		check("SAMPLE = 2048", SAMP16(m), 2048);
		check("RESRDY and SAMPRDY both set", m->data[A + INTFLAGS] & 3, 3);
		check("START self-clears (IMMEDIATE -> STOP)", m->data[A + COMMAND] & 7, 0);
		check("ADCBUSY cleared", m->data[A + STATUS], 0);
		cpu_write(m, A + RESULT, 0x55);
		cpu_write(m, A + SAMPLE, 0x55);
		cpu_write(m, A + STATUS, 0x01);
		check("RESULT is read-only", RES32(m), 2048);
		check("SAMPLE is read-only", SAMP16(m), 2048);
		check("STATUS is read-only", m->data[A + STATUS], 0);
		cpu_read(m, A + SAMPLE);
		check("reading SAMPLE clears SAMPRDY", m->data[A + INTFLAGS] & 3, 1);
		cpu_read(m, A + RESULT);
		check("reading RESULT clears RESRDY", m->data[A + INTFLAGS] & 3, 0);

		/* Single 8-bit: 1650/3300*256 = 128; (SAMPDUR+11) clocks. */
		cpu_write(m, A + COMMAND, (0 << 4) | 1);
		t = run_until_set(m, A + INTFLAGS, F_RES, 400);
		check("8-bit conversion time = init + 22 cyc", t >= 54 && t <= 57, 1);
		check("8-bit result = 128", RES32(m), 128);
		ADC_CLEAR(m);

		/* SAMPDUR adds (SAMPDUR) ADC clocks. */
		cpu_write(m, A + CTRLE, 10);
		cpu_write(m, A + COMMAND, (1 << 4) | 1);
		t = run_until_set(m, A + INTFLAGS, F_RES, 600);
		check("SAMPDUR=10 adds 20 cycles at DIV2", t >= 82 && t <= 85, 1);
		ADC_CLEAR(m);
		cpu_write(m, A + CTRLE, 0);

		/* CTRLB.PRESC = DIV8 (3): the ADC clock is 4x slower. */
		cpu_write(m, A + CTRLB, 3);
		cpu_write(m, A + COMMAND, (1 << 4) | 1);
		t = run_until_set(m, A + INTFLAGS, F_RES, 800);
		check("PRESC DIV8: 15 clocks = 120 cyc + init", t >= 152 && t <= 156, 1);
		ADC_CLEAR(m);
		cpu_write(m, A + CTRLB, 0);

		/* LOWLAT removes the initialisation time once the modules are warm. */
		cpu_write(m, A + CTRLA, 0x00);
		cpu_write(m, A + CTRLA, 0x21);			/* ENABLE | LOWLAT */
		cpu_write(m, A + COMMAND, (1 << 4) | 1);
		t = run_until_set(m, A + INTFLAGS, F_RES, 400);
		check("LOWLAT: first conversion still initialises", t >= 62 && t <= 65, 1);
		ADC_CLEAR(m);
		cpu_write(m, A + COMMAND, (1 << 4) | 1);
		t = run_until_set(m, A + INTFLAGS, F_RES, 400);
		check("LOWLAT: second conversion has no init (30 cyc)", t >= 29 && t <= 32, 1);
		ADC_CLEAR(m);
		cpu_write(m, A + CTRLA, 0x00);
		cpu_write(m, A + CTRLA, 0x01);

		/* LEFTADJ: 12-bit result and sample shifted left by 4; 8-bit is not. */
		cpu_write(m, A + CTRLF, 0x10);
		cpu_write(m, A + COMMAND, (1 << 4) | 1);
		run_until_set(m, A + INTFLAGS, F_RES, 400);
		check("LEFTADJ result = 2048 << 4", RES32(m), 0x8000);
		check("LEFTADJ sample = 2048 << 4", SAMP16(m), 0x8000);
		ADC_CLEAR(m);
		cpu_write(m, A + COMMAND, (0 << 4) | 1);
		run_until_set(m, A + INTFLAGS, F_RES, 400);
		check("LEFTADJ ignored in 8-bit mode", RES32(m), 128);
		ADC_CLEAR(m);
		cpu_write(m, A + CTRLF, 0x00);

		/* Differential: (VINP - VINN)/VREF * 2048, two's complement, sign-extended. */
		avr_raise_irq(ain3, 2000);
		avr_raise_irq(ain5, 500);
		cpu_write(m, A + MUXNEG, 5);
		cpu_write(m, A + COMMAND, 0x80 | (1 << 4) | 1);
		run_until_set(m, A + INTFLAGS, F_RES, 400);
		check("diff 12-bit (2000-500mV) = 930", RES32(m), 930);
		ADC_CLEAR(m);
		avr_raise_irq(ain3, 500);
		avr_raise_irq(ain5, 2000);
		cpu_write(m, A + COMMAND, 0x80 | (1 << 4) | 1);
		run_until_set(m, A + INTFLAGS, F_RES, 400);
		check("diff 12-bit (500-2000mV) = -931", RES32(m), -931);
		check("diff result sign-extends into RESULT[31:24]", m->data[A + RESULT + 3], 0xff);
		check("diff sample is 16-bit two's complement", SAMP16(m), (uint16_t)-931);
		ADC_CLEAR(m);
		avr_raise_irq(ain3, 3300);
		avr_raise_irq(ain5, 0);
		cpu_write(m, A + COMMAND, 0x80 | (1 << 4) | 1);
		run_until_set(m, A + INTFLAGS, F_RES, 400);
		check("diff positive full scale clamps to 2047", RES32(m), 2047);
		ADC_CLEAR(m);
		avr_raise_irq(ain3, 0);
		avr_raise_irq(ain5, 3300);
		cpu_write(m, A + COMMAND, 0x80 | (0 << 4) | 1);
		run_until_set(m, A + INTFLAGS, F_RES, 400);
		check("diff 8-bit negative full scale = -128", RES32(m), -128);
		ADC_CLEAR(m);

		/* PGA: gain 4x (GAIN=2), VIA=PGA: 300 mV -> 1200 mV -> 1489. Timing adds
		 * the 20 us PGA init and ADCPGASAMPDUR (20 clocks, the reset default). */
		avr_raise_irq(ain3, 300);
		cpu_write(m, A + PGACTRL, (2 << 5) | 0x04 | 0x01);
		cpu_write(m, A + MUXPOS, 0x40 | 3);
		check("VIA is shared: MUXNEG.VIA mirrors MUXPOS.VIA", m->data[A + MUXNEG] & 0xc0, 0x40);
		cpu_write(m, A + COMMAND, (1 << 4) | 1);
		t = run_until_set(m, A + INTFLAGS, F_RES, 600);
		check("PGA single time = 20us init + 35.5 clocks", t >= 136 && t <= 140, 1);
		check("PGA 4x: 300 mV -> 1489", RES32(m), 1489);
		ADC_CLEAR(m);
		cpu_write(m, A + PGACTRL, (4 << 5) | 0x04 | 0x01);	/* 16x */
		cpu_write(m, A + COMMAND, (1 << 4) | 1);
		run_until_set(m, A + INTFLAGS, F_RES, 600);
		check("PGA 16x: 300 mV -> 4095 (clamped)", RES32(m), 4095);
		ADC_CLEAR(m);
		cpu_write(m, A + PGACTRL, (2 << 5) | 0x04);		/* PGAEN = 0: VIA ignored */
		cpu_write(m, A + COMMAND, (1 << 4) | 1);
		run_until_set(m, A + INTFLAGS, F_RES, 600);
		check("PGAEN=0: direct path, 300 mV -> 372", RES32(m), 372);
		ADC_CLEAR(m);
		cpu_write(m, A + MUXPOS, 3);
		cpu_write(m, A + PGACTRL, 0x04);
		avr_raise_irq(ain3, 1650);

		/* Reference selection (CTRLC.REFSEL), independent of VREF on this family. */
		cpu_write(m, A + CTRLC, 0x05);			/* 2.048 V */
		cpu_write(m, A + COMMAND, (1 << 4) | 1);
		run_until_set(m, A + INTFLAGS, F_RES, 600);
		check("REFSEL 2.048V: 1650 mV -> 3300", RES32(m), 3300);
		ADC_CLEAR(m);
		cpu_write(m, A + CTRLC, 0x04);			/* 1.024 V: input clamps */
		cpu_write(m, A + COMMAND, (1 << 4) | 1);
		run_until_set(m, A + INTFLAGS, F_RES, 600);
		check("REFSEL 1.024V: 1650 mV clamps to 4095", RES32(m), 4095);
		ADC_CLEAR(m);
		cpu_write(m, A + CTRLC, 0x02);			/* external VREFA */
		avr_raise_irq(avr_io_getirq(m, AVR_IOCTL_ADC2_GETIRQ('0'), AVR_ADC2_IRQ_VREFA), 3000);
		cpu_write(m, A + COMMAND, (1 << 4) | 1);
		run_until_set(m, A + INTFLAGS, F_RES, 600);
		check("REFSEL VREFA=3000: 1650 mV -> 2252", RES32(m), 2252);
		ADC_CLEAR(m);
		cpu_write(m, A + CTRLC, 0x00);
		avr_raise_irq(avr_io_getirq(m, AVR_IOCTL_ADC2_GETIRQ('0'), AVR_ADC2_IRQ_VDD), 5000);
		cpu_write(m, A + COMMAND, (1 << 4) | 1);
		run_until_set(m, A + INTFLAGS, F_RES, 600);
		check("REFSEL VDD=5000: 1650 mV -> 1351", RES32(m), 1351);
		ADC_CLEAR(m);
		avr_raise_irq(avr_io_getirq(m, AVR_IOCTL_ADC2_GETIRQ('0'), AVR_ADC2_IRQ_VDD), 3300);

		/* Internal inputs: VDD/10, DACREF0 (AC0), GND. */
		cpu_write(m, A + MUXPOS, 0x31);
		cpu_write(m, A + COMMAND, (1 << 4) | 1);
		run_until_set(m, A + INTFLAGS, F_RES, 600);
		check("VDDDIV10: 330 mV -> 409", RES32(m), 409);
		ADC_CLEAR(m);
		cpu_write(m, 0x680 + ACR_MUXCTRLA, 0x03);
		cpu_write(m, 0x0a0 + VREFR_CTRLA, 0x02);		/* AC0REFSEL = 2.5 V */
		cpu_write(m, 0x680 + ACR_DACREF, 0x80);			/* 1250 mV */
		cpu_write(m, A + MUXPOS, 0x33);
		cpu_write(m, A + COMMAND, (1 << 4) | 1);
		t = run_until_set(m, A + INTFLAGS, F_RES, 600);
		check("DACREF0: 1250 mV -> 1551", RES32(m), 1551);
		check("DACREF0 input costs 35us init", t >= 30 + 115 && t <= 30 + 118, 1);
		ADC_CLEAR(m);
		cpu_write(m, A + MUXPOS, 0x30);
		cpu_write(m, A + COMMAND, (1 << 4) | 1);
		run_until_set(m, A + INTFLAGS, F_RES, 600);
		check("GND input -> 0", RES32(m), 0);
		ADC_CLEAR(m);

		/* Temperature sensor: firmware's T = ((RESULT>>2) - off) * gain / 256. */
		m->data[0x1120] = 180;				/* SIGROW.TEMPSENSE0 (gain) */
		m->data[0x1121] = (uint8_t)-20;			/* SIGROW.TEMPSENSE1 (offset) */
		cpu_write(m, A + CTRLC, 0x04);			/* 1.024 V reference */
		cpu_write(m, A + MUXPOS, 0x32);
		cpu_write(m, A + COMMAND, (1 << 4) | 1);
		t = run_until_set(m, A + INTFLAGS, F_RES, 800);
		check("tempsense needs the 60us reference init", t >= 200 && t <= 204 + 30, 1);
		{
			int32_t adc = RES32(m) >> 2;
			int32_t k = (((adc - (int8_t)m->data[0x1121]) * m->data[0x1120]) + 0x80) >> 8;
			check("tempsense recovers 298 K (+/-1)", k >= 297 && k <= 299, 1);
		}
		ADC_CLEAR(m);
		avr_raise_irq(avr_io_getirq(m, AVR_IOCTL_ADC2_GETIRQ('0'), AVR_ADC2_CH_TEMPSENSE), 358);
		cpu_write(m, A + COMMAND, (1 << 4) | 1);
		run_until_set(m, A + INTFLAGS, F_RES, 800);
		{
			int32_t adc = RES32(m) >> 2;
			int32_t k = (((adc - (int8_t)m->data[0x1121]) * m->data[0x1120]) + 0x80) >> 8;
			check("tempsense follows 358 K (+/-1)", k >= 357 && k <= 359, 1);
		}
		ADC_CLEAR(m);
		cpu_write(m, A + CTRLC, 0x00);
		cpu_write(m, A + MUXPOS, 3);
	}


	printf("== tinyAVR 2-series ADC0: accumulation, FREERUN, triggers ==\n");
	{
		enum { CTRLA = 0, CTRLB = 1, CTRLC = 2, CTRLD = 3, INTCTRL = 4, INTFLAGS = 5,
			   STATUS = 6, CTRLE = 8, CTRLF = 9, COMMAND = 0x0a, PGACTRL = 0x0b,
			   MUXPOS = 0x0c, MUXNEG = 0x0d, RESULT = 0x10, SAMPLE = 0x14 };
		enum { F_RES = 0x01, F_SAMP = 0x02, F_WCMP = 0x04, F_RESOVR = 0x08,
			   F_SAMPOVR = 0x10, F_TRIGOVR = 0x20 };
		const avr_io_addr_t A = 0x600;
		avr_t *m = avr_make_mcu_by_name("attiny3226");
		if (!m) { printf("cannot make attiny3226 core\n"); return 2; }
		m->log = LOG_ERROR;
		avr_init(m);
		memset(m->flash, 0, 0x2000);
		avr_irq_t *ain3 = avr_io_getirq(m, AVR_IOCTL_ADC2_GETIRQ('0'), 3);
		avr_irq_t *ain5 = avr_io_getirq(m, AVR_IOCTL_ADC2_GETIRQ('0'), 5);
		avr_raise_irq(ain3, 1650);
		cpu_write(m, A + MUXPOS, 3);
		cpu_write(m, A + CTRLA, 0x01);

		/* Series, SAMPNUM=2 (4 samples): one conversion per trigger; RESRDY
		 * only after the 4th, with the full 24-bit accumulation. */
		cpu_write(m, A + CTRLF, 2);
		for (int i = 0; i < 4; i++) {
			char w[64];
			cpu_write(m, A + COMMAND, (2 << 4) | 1);
			run_until_set(m, A + INTFLAGS, F_SAMP, 400);
			snprintf(w, sizeof(w), "series trigger %d: SAMPRDY", i + 1);
			check(w, m->data[A + INTFLAGS] & F_SAMP, F_SAMP);
			snprintf(w, sizeof(w), "series trigger %d: RESRDY only on the 4th", i + 1);
			check(w, m->data[A + INTFLAGS] & F_RES, i == 3 ? F_RES : 0);
			cpu_read(m, A + SAMPLE);
		}
		check("series x4 accumulation = 4*2048", RES32(m), 8192);
		check("series sample = last conversion", SAMP16(m), 2048);
		cpu_read(m, A + RESULT);
		cpu_write(m, A + COMMAND, (2 << 4) | 1);
		run_until_set(m, A + INTFLAGS, F_SAMP, 400);
		check("series restarts a new accumulation (no RESRDY)", m->data[A + INTFLAGS] & F_RES, 0);
		cpu_read(m, A + SAMPLE);

		/* Leaving accumulation for Single mode resets the accumulator. */
		cpu_write(m, A + COMMAND, (2 << 4) | 1);		/* 2nd sample of a new series */
		run_until_set(m, A + INTFLAGS, F_SAMP, 400);
		cpu_read(m, A + SAMPLE);
		cpu_write(m, A + COMMAND, (1 << 4) | 0);		/* MODE=Single, STOP */
		for (int i = 0; i < 4; i++) {
			cpu_write(m, A + COMMAND, (2 << 4) | 1);
			run_until_set(m, A + INTFLAGS, F_SAMP, 400);
			cpu_read(m, A + SAMPLE);
			if (i == 2)
				check("accumulator reset: no RESRDY after 3 triggers",
					  m->data[A + INTFLAGS] & F_RES, 0);
		}
		check("accumulator reset: RESRDY after the 4th trigger",
			  m->data[A + INTFLAGS] & F_RES, F_RES);
		check("accumulator was reset by the Single-mode write", RES32(m), 8192);
		cpu_read(m, A + RESULT);

		/* Burst, SAMPNUM=3 (8 samples): one trigger, 10us init + (14*8+1.5) clocks. */
		cpu_write(m, A + CTRLA, 0x00);
		cpu_write(m, A + CTRLA, 0x01);
		cpu_write(m, A + CTRLF, 3);
		cpu_write(m, A + COMMAND, (4 << 4) | 1);
		long t = run_until_set(m, A + INTFLAGS, F_RES, 1000);
		check("burst x8 time = 33 + 227 cycles", t >= 258 && t <= 262, 1);
		check("burst x8 accumulation = 8*2048", RES32(m), 16384);
		check("burst sample = last conversion", SAMP16(m), 2048);
		check("unread samples overwritten -> SAMPOVR", !!(m->data[A + INTFLAGS] & F_SAMPOVR), 1);
		ADC_CLEAR(m);
		check("burst START returns to STOP", m->data[A + COMMAND] & 7, 0);

		/* Burst with scaling: SAMPNUM=6 (64 samples) -> acc >> (6-4). */
		cpu_write(m, A + CTRLF, 6);
		cpu_write(m, A + COMMAND, (5 << 4) | 1);
		run_until_set(m, A + INTFLAGS, F_RES, 6000);
		check("burst x64 scaled = (64*2048) >> 2", RES32(m), 32768);
		ADC_CLEAR(m);
		/* < 16 samples: not shifted; LEFTADJ justifies bit 15 as the MSb. */
		cpu_write(m, A + CTRLF, 2);
		cpu_write(m, A + COMMAND, (5 << 4) | 1);
		run_until_set(m, A + INTFLAGS, F_RES, 1000);
		check("burst x4 scaled, no LEFTADJ = 8192", RES32(m), 8192);
		ADC_CLEAR(m);
		cpu_write(m, A + CTRLF, 2 | 0x10);
		cpu_write(m, A + COMMAND, (5 << 4) | 1);
		run_until_set(m, A + INTFLAGS, F_RES, 1000);
		check("burst x4 scaled, LEFTADJ -> bit15 is MSb", RES32(m), 32768);
		ADC_CLEAR(m);
		/* Unscaled accumulation ignores LEFTADJ. */
		cpu_write(m, A + COMMAND, (4 << 4) | 1);
		run_until_set(m, A + INTFLAGS, F_RES, 1000);
		check("unscaled accumulation ignores LEFTADJ", RES32(m), 8192);
		ADC_CLEAR(m);
		/* SAMPNUM=0: a burst of one. */
		cpu_write(m, A + CTRLF, 0);
		cpu_write(m, A + COMMAND, (4 << 4) | 1);
		run_until_set(m, A + INTFLAGS, F_RES, 1000);
		check("burst SAMPNUM=0 = one sample", RES32(m), 2048);
		ADC_CLEAR(m);
		/* The widest accumulation: 1024 samples need 22 bits, no overflow. */
		cpu_write(m, A + CTRLF, 10);
		cpu_write(m, A + COMMAND, (4 << 4) | 1);
		run_until_set(m, A + INTFLAGS, F_RES, 40000);
		check("burst x1024 = 1024*2048", RES32(m), 1024 * 2048);
		ADC_CLEAR(m);
		/* Differential accumulation sign-extends. */
		avr_raise_irq(ain3, 500);
		avr_raise_irq(ain5, 2000);
		cpu_write(m, A + MUXNEG, 5);
		cpu_write(m, A + CTRLF, 2);
		cpu_write(m, A + COMMAND, 0x80 | (4 << 4) | 1);
		run_until_set(m, A + INTFLAGS, F_RES, 1000);
		check("diff burst x4 = 4*-931", RES32(m), -3724);
		ADC_CLEAR(m);
		cpu_write(m, A + COMMAND, 0x80 | (5 << 4) | 1);
		run_until_set(m, A + INTFLAGS, F_RES, 1000);
		check("diff burst x4 scaled = -3724 (arith)", RES32(m), -3724);
		ADC_CLEAR(m);
		avr_raise_irq(ain3, 1650);
		cpu_write(m, A + CTRLF, 0);

		/* FREERUN restarts Single immediately (no init): ~30-cycle cadence. */
		cpu_write(m, A + CTRLF, 0x20);
		cpu_write(m, A + COMMAND, (1 << 4) | 1);
		int hits = 0;
		long last = -1, maxgap = 0, mingap = 1 << 30;
		for (int i = 0; i < 6000 && hits < 5; i++) {
			avr_run(m);
			if (m->data[A + INTFLAGS] & F_RES) {
				if (last >= 0) {
					long g = (long)m->cycle - last;
					if (g > maxgap) maxgap = g;
					if (g < mingap) mingap = g;
				}
				last = (long)m->cycle;
				hits++;
				ADC_CLEAR(m);
			}
		}
		check("FREERUN keeps converting", hits >= 5, 1);
		check("FREERUN cadence = 30 cycles (no re-init)", mingap >= 29 && maxgap <= 31, 1);
		check("FREERUN keeps START=IMMEDIATE", m->data[A + COMMAND] & 7, 1);
		cpu_write(m, A + COMMAND, (1 << 4) | 0);		/* STOP */
		ADC_CLEAR(m);
		long quiet = run_until_set(m, A + INTFLAGS, F_RES | F_SAMP, 300);
		check("STOP ends FREERUN", quiet, -1);
		check("STOP clears ADCBUSY", m->data[A + STATUS], 0);

		/* FREERUN is ignored in Series mode: one trigger, one conversion. */
		cpu_write(m, A + CTRLF, 0x20 | 2);
		cpu_write(m, A + COMMAND, (2 << 4) | 1);
		run_until_set(m, A + INTFLAGS, F_SAMP, 400);
		ADC_CLEAR(m);
		quiet = run_until_set(m, A + INTFLAGS, F_SAMP, 200);
		check("FREERUN has no effect in Series mode", quiet, -1);
		cpu_write(m, A + CTRLF, 0);
		cpu_write(m, A + COMMAND, (1 << 4) | 0);

		/* FREERUN with Burst accumulation. */
		cpu_write(m, A + CTRLF, 0x20 | 1);
		cpu_write(m, A + COMMAND, (4 << 4) | 1);
		hits = 0;
		for (int i = 0; i < 4000 && hits < 3; i++) {
			avr_run(m);
			if (m->data[A + INTFLAGS] & F_RES) { hits++; ADC_CLEAR(m); }
		}
		check("FREERUN burst repeats", hits, 3);
		cpu_write(m, A + COMMAND, (4 << 4) | 0);
		ADC_CLEAR(m);
		cpu_write(m, A + CTRLF, 0);

		/* Trigger sources: START=MUXPOS_WRITE / MUXNEG_WRITE start on a mux write. */
		cpu_write(m, A + MUXNEG, 0);
		cpu_write(m, A + COMMAND, (1 << 4) | 2);
		quiet = run_until_set(m, A + INTFLAGS, F_RES, 100);
		check("MUXPOS_WRITE: nothing starts by itself", quiet, -1);
		cpu_write(m, A + MUXPOS, 3);
		check("MUXPOS write triggers a conversion", m->data[A + STATUS], 1);
		run_until_set(m, A + INTFLAGS, F_RES, 400);
		check("MUXPOS_WRITE result = 2048", RES32(m), 2048);
		check("MUXPOS_WRITE keeps START armed", m->data[A + COMMAND] & 7, 2);
		ADC_CLEAR(m);
		cpu_write(m, A + MUXNEG, 5);
		quiet = run_until_set(m, A + INTFLAGS, F_RES, 100);
		check("MUXPOS_WRITE ignores MUXNEG writes", quiet, -1);
		cpu_write(m, A + COMMAND, (1 << 4) | 3);
		cpu_write(m, A + MUXNEG, 0);
		check("MUXNEG write triggers a conversion", m->data[A + STATUS], 1);
		run_until_set(m, A + INTFLAGS, F_RES, 400);
		ADC_CLEAR(m);
		cpu_write(m, A + COMMAND, (1 << 4) | 0);

		/* Triggering while converting: TRIGOVR, the trigger is ignored. */
		cpu_write(m, A + COMMAND, (1 << 4) | 1);
		cpu_write(m, A + COMMAND, (1 << 4) | 1);
		check("trigger during conversion sets TRIGOVR", !!(m->data[A + INTFLAGS] & F_TRIGOVR), 1);
		run_until_set(m, A + INTFLAGS, F_RES, 400);
		check("first conversion still completes", RES32(m), 2048);
		long extra = run_until_set(m, A + INTFLAGS, F_RES | 0x80, 100);
		(void)extra;
		ADC_CLEAR(m);
		quiet = run_until_set(m, A + INTFLAGS, F_RES, 200);
		check("ignored trigger started nothing", quiet, -1);

		/* Unread result/sample overwritten by the next conversion. */
		ADC_CLEAR(m);
		cpu_write(m, A + COMMAND, (1 << 4) | 1);
		run_until_set(m, A + INTFLAGS, F_RES, 400);
		check("first conversion: no overwrite flags", m->data[A + INTFLAGS] & (F_RESOVR | F_SAMPOVR), 0);
		cpu_write(m, A + COMMAND, (1 << 4) | 1);
		run_until_set(m, A + INTFLAGS, F_RESOVR, 400);
		check("unread RESULT overwritten -> RESOVR", !!(m->data[A + INTFLAGS] & F_RESOVR), 1);
		check("unread SAMPLE overwritten -> SAMPOVR", !!(m->data[A + INTFLAGS] & F_SAMPOVR), 1);
		ADC_CLEAR(m);

		/* STOP aborts a running conversion; disabling aborts too. */
		cpu_write(m, A + COMMAND, (1 << 4) | 1);
		for (int i = 0; i < 10; i++) avr_run(m);
		cpu_write(m, A + COMMAND, (1 << 4) | 0);
		check("STOP aborts: ADCBUSY clear", m->data[A + STATUS], 0);
		quiet = run_until_set(m, A + INTFLAGS, F_RES | F_SAMP, 300);
		check("STOP aborts: no result arrives", quiet, -1);
		cpu_write(m, A + COMMAND, (1 << 4) | 1);
		for (int i = 0; i < 10; i++) avr_run(m);
		cpu_write(m, A + CTRLA, 0x00);
		quiet = run_until_set(m, A + INTFLAGS, F_RES | F_SAMP, 300);
		check("ENABLE=0 aborts the conversion", quiet, -1);
		check("ENABLE=0 clears ADCBUSY", m->data[A + STATUS], 0);
		/* IMMEDIATE while disabled reads back as STOP and starts nothing. */
		cpu_write(m, A + COMMAND, (1 << 4) | 1);
		check("IMMEDIATE with ADC disabled reads back STOP", m->data[A + COMMAND] & 7, 0);
		quiet = run_until_set(m, A + INTFLAGS, F_RES | F_SAMP, 300);
		check("IMMEDIATE with ADC disabled starts nothing", quiet, -1);
		cpu_write(m, A + CTRLA, 0x01);
	}

	printf("== tinyAVR 2-series ADC0: window comparator, vectors, events ==\n");
	{
		enum { CTRLA = 0, CTRLB = 1, CTRLC = 2, CTRLD = 3, INTCTRL = 4, INTFLAGS = 5,
			   STATUS = 6, CTRLE = 8, CTRLF = 9, COMMAND = 0x0a, PGACTRL = 0x0b,
			   MUXPOS = 0x0c, MUXNEG = 0x0d, RESULT = 0x10, SAMPLE = 0x14,
			   WINLT = 0x1c, WINHT = 0x1e };
		enum { F_RES = 0x01, F_SAMP = 0x02, F_WCMP = 0x04, F_RESOVR = 0x08,
			   F_SAMPOVR = 0x10, F_TRIGOVR = 0x20 };
		const avr_io_addr_t A = 0x600;
		avr_t *m = avr_make_mcu_by_name("attiny3226");
		if (!m) { printf("cannot make attiny3226 core\n"); return 2; }
		m->log = LOG_ERROR;
		avr_init(m);
		memset(m->flash, 0, 0x2000);
		avr_irq_t *ain3 = avr_io_getirq(m, AVR_IOCTL_ADC2_GETIRQ('0'), 3);
		avr_irq_t *ain5 = avr_io_getirq(m, AVR_IOCTL_ADC2_GETIRQ('0'), 5);
		avr_raise_irq(ain3, 1650);
		cpu_write(m, A + MUXPOS, 3);
		cpu_write(m, A + CTRLA, 0x21);		/* LOWLAT: keep the loops short */

#define WIN(lt, ht) do { \
			cpu_write(m, A + WINLT, (lt) & 0xff); cpu_write(m, A + WINLT + 1, (lt) >> 8); \
			cpu_write(m, A + WINHT, (ht) & 0xff); cpu_write(m, A + WINHT + 1, (ht) >> 8); \
		} while (0)
#define CONVERT(cmd) do { \
			ADC_CLEAR(m); cpu_write(m, A + COMMAND, (cmd)); \
			run_until_set(m, A + INTFLAGS, F_RES, 40000); \
		} while (0)
#define WCMP_SET() (!!(m->data[A + INTFLAGS] & F_WCMP))

		/* Window modes on the RESULT (2048). Comparisons are strict. */
		cpu_write(m, A + CTRLD, 2);	WIN(0, 1000);	CONVERT((1 << 4) | 1);
		check("ABOVE: 2048 > 1000", WCMP_SET(), 1);
		WIN(0, 2048);	CONVERT((1 << 4) | 1);
		check("ABOVE: 2048 > 2048 is false", WCMP_SET(), 0);
		cpu_write(m, A + CTRLD, 1);	WIN(3000, 0);	CONVERT((1 << 4) | 1);
		check("BELOW: 2048 < 3000", WCMP_SET(), 1);
		WIN(2048, 0);	CONVERT((1 << 4) | 1);
		check("BELOW: 2048 < 2048 is false", WCMP_SET(), 0);
		cpu_write(m, A + CTRLD, 3);	WIN(2048, 3000);	CONVERT((1 << 4) | 1);
		check("INSIDE: LT < 2048 is false at the edge", WCMP_SET(), 0);
		WIN(2047, 3000);	CONVERT((1 << 4) | 1);
		check("INSIDE: 2047 < 2048 < 3000", WCMP_SET(), 1);
		cpu_write(m, A + CTRLD, 4);	WIN(2048, 2048);	CONVERT((1 << 4) | 1);
		check("OUTSIDE: equal to both thresholds is not outside", WCMP_SET(), 0);
		WIN(2049, 3000);	CONVERT((1 << 4) | 1);
		check("OUTSIDE: 2048 < 2049", WCMP_SET(), 1);
		cpu_write(m, A + CTRLD, 0);	WIN(5000, 0);	CONVERT((1 << 4) | 1);
		check("WINCM=NONE: window disabled", WCMP_SET(), 0);
		cpu_write(m, A + CTRLD, 5);	CONVERT((1 << 4) | 1);
		check("reserved WINCM: no match", WCMP_SET(), 0);

		/* WINSRC picks the compared register when accumulating (result 8192,
		 * sample 2048). */
		cpu_write(m, A + CTRLF, 2);
		WIN(0, 4000);
		cpu_write(m, A + CTRLD, 2);			/* ABOVE, source RESULT */
		CONVERT((4 << 4) | 1);
		check("WINSRC=RESULT: 8192 > 4000", WCMP_SET(), 1);
		cpu_write(m, A + CTRLD, 0x08 | 2);		/* ABOVE, source SAMPLE */
		CONVERT((4 << 4) | 1);
		check("WINSRC=SAMPLE: 2048 > 4000 is false", WCMP_SET(), 0);
		cpu_write(m, A + CTRLF, 0);

		/* Differential values compare as signed (here -931 < -1). */
		avr_raise_irq(ain3, 500);
		avr_raise_irq(ain5, 2000);
		cpu_write(m, A + MUXNEG, 5);
		cpu_write(m, A + CTRLD, 1);			/* BELOW */
		WIN(0xffff, 0);
		CONVERT(0x80 | (1 << 4) | 1);
		check("differential: -931 BELOW -1 (signed)", WCMP_SET(), 1);
		avr_raise_irq(ain3, 1650);
		cpu_write(m, A + MUXNEG, 0);
		cpu_write(m, A + CTRLD, 0);
		ADC_CLEAR(m);

		/* Interrupt routing: RESRDY -> 22, SAMPRDY -> 23, ERROR -> 21 (ADC0_*_vect_num). */
		cpu_write(m, A + INTCTRL, F_RES);
		CONVERT((1 << 4) | 1);
		check("RESRDY interrupt -> vector 22", take_vector(m), 22);
		check("no second request after servicing", take_vector(m), -1);
		ADC_CLEAR(m);
		cpu_write(m, A + INTCTRL, F_SAMP);
		CONVERT((1 << 4) | 1);
		check("SAMPRDY interrupt -> vector 23", take_vector(m), 23);
		ADC_CLEAR(m);
		cpu_write(m, A + INTCTRL, F_RES | F_SAMP);
		CONVERT((1 << 4) | 1);
		check("both enabled: lower vector (RESRDY=22) first", take_vector(m), 22);
		check("both enabled: then SAMPRDY=23", take_vector(m), 23);
		ADC_CLEAR(m);
		cpu_write(m, A + INTCTRL, 0);
		CONVERT((1 << 4) | 1);
		check("flags set but nothing enabled: no interrupt", take_vector(m), -1);
		cpu_write(m, A + INTCTRL, F_RES);
		check("enabling an already-set flag requests it", take_vector(m), 22);
		/* Reading RESULT clears RESRDY and withdraws a pending request. */
		ADC_CLEAR(m);
		CONVERT((1 << 4) | 1);
		cpu_read(m, A + RESULT);
		check("reading RESULT withdraws the RESRDY request", take_vector(m), -1);
		/* W1C also withdraws it. */
		CONVERT((1 << 4) | 1);
		cpu_write(m, A + INTFLAGS, F_RES);
		check("W1C withdraws the RESRDY request", take_vector(m), -1);

		/* WCMP uses the RESRDY vector for WINSRC=RESULT, SAMPRDY for SAMPLE. */
		ADC_CLEAR(m);
		WIN(0, 1000);
		cpu_write(m, A + INTCTRL, F_WCMP);
		cpu_write(m, A + CTRLD, 2);			/* ABOVE, RESULT */
		CONVERT((1 << 4) | 1);
		check("WCMP (WINSRC=RESULT) -> vector 22", take_vector(m), 22);
		ADC_CLEAR(m);
		cpu_write(m, A + CTRLD, 0x08 | 2);		/* ABOVE, SAMPLE */
		CONVERT((1 << 4) | 1);
		check("WCMP (WINSRC=SAMPLE) -> vector 23", take_vector(m), 23);
		cpu_write(m, A + CTRLD, 0);
		ADC_CLEAR(m);

		/* ERROR vector: trigger overrun, sample overwrite, result overwrite. */
		cpu_write(m, A + INTCTRL, F_TRIGOVR);
		cpu_write(m, A + COMMAND, (1 << 4) | 1);
		cpu_write(m, A + COMMAND, (1 << 4) | 1);
		check("TRIGOVR interrupt -> vector 21", take_vector(m), 21);
		run_until_set(m, A + INTFLAGS, F_RES, 400);
		ADC_CLEAR(m);
		cpu_write(m, A + INTCTRL, F_RESOVR);
		CONVERT((1 << 4) | 1);
		CONVERT((1 << 4) | 1);			/* ADC_CLEAR inside: unread? flags cleared */
		cpu_write(m, A + COMMAND, (1 << 4) | 1);
		run_until_set(m, A + INTFLAGS, F_RESOVR, 400);
		check("RESOVR interrupt -> vector 21", take_vector(m), 21);
		ADC_CLEAR(m);
		cpu_write(m, A + INTCTRL, F_SAMPOVR);
		CONVERT((1 << 4) | 1);
		cpu_write(m, A + COMMAND, (1 << 4) | 1);
		run_until_set(m, A + INTFLAGS, F_SAMPOVR, 400);
		check("SAMPOVR interrupt -> vector 21", take_vector(m), 21);
		cpu_write(m, A + INTCTRL, 0);
		ADC_CLEAR(m);

		/* Event generators: RES / SAMP / WCMP pulse EVSYS channels. */
		const avr_io_addr_t E = 0x180;
		int n_res = 0, n_samp = 0, n_wcmp = 0;
		cpu_write(m, E + 0x11, 0x24);			/* CHANNEL1 = ADC0 RES */
		cpu_write(m, E + 0x12, 0x25);			/* CHANNEL2 = ADC0 SAMP */
		cpu_write(m, E + 0x13, 0x26);			/* CHANNEL3 = ADC0 WCMP */
		cpu_write(m, E + 0x20 + 0, 2);			/* USERCCLLUT0A -> CHANNEL1 */
		cpu_write(m, E + 0x20 + 1, 3);			/* USERCCLLUT0B -> CHANNEL2 */
		cpu_write(m, E + 0x20 + 2, 4);			/* USERCCLLUT1A -> CHANNEL3 */
		avr_irq_register_notify(avr_io_getirq(m, AVR_IOCTL_EVSYS_GETIRQ('0'),
							AVR_EVSYS_IRQ_USER0 + 0), evsys_count_param_hook, &n_res);
		avr_irq_register_notify(avr_io_getirq(m, AVR_IOCTL_EVSYS_GETIRQ('0'),
							AVR_EVSYS_IRQ_USER0 + 1), evsys_count_param_hook, &n_samp);
		avr_irq_register_notify(avr_io_getirq(m, AVR_IOCTL_EVSYS_GETIRQ('0'),
							AVR_EVSYS_IRQ_USER0 + 2), evsys_count_param_hook, &n_wcmp);
		WIN(0, 1000);
		cpu_write(m, A + CTRLD, 2);
		CONVERT((1 << 4) | 1);
		check("ADC RES event pulses once", n_res, 1);
		check("ADC SAMP event pulses once", n_samp, 1);
		check("ADC WCMP event pulses once (window hit)", n_wcmp, 1);
		cpu_write(m, A + CTRLD, 0);
		CONVERT((1 << 4) | 1);
		check("no WCMP event without a window match", n_wcmp, 1);
		check("RES event per conversion", n_res, 2);

		/* Event user: START=EVENT_TRIGGER starts a conversion on a rising event edge
		 * (AC0 OUT -> CHANNEL0 -> USERADC0START). */
		avr_irq_t *ainp0 = avr_io_getirq(m, AVR_IOCTL_AC_GETIRQ('0'), AVR_AC_IRQ_AINP0);
		avr_irq_t *ainn0 = avr_io_getirq(m, AVR_IOCTL_AC_GETIRQ('0'), AVR_AC_IRQ_AINN0);
		cpu_write(m, 0x680 + ACR_MUXCTRLA, 0x00);
		avr_raise_irq(ainn0, 1000);
		avr_raise_irq(ainp0, 0);
		cpu_write(m, 0x680 + ACR_CTRLA, 0x01);
		cpu_write(m, E + 0x10, 0x20);			/* CHANNEL0 = AC0 OUT */
		cpu_write(m, E + 0x28, 1);			/* USERADC0START -> CHANNEL0 */
		ADC_CLEAR(m);
		cpu_write(m, A + COMMAND, (1 << 4) | 4);	/* START = EVENT_TRIGGER */
		check("EVENT_TRIGGER waits for an event", m->data[A + STATUS], 0);
		avr_raise_irq(ainp0, 2000);			/* AC0 output rises */
		check("rising event edge starts a conversion", m->data[A + STATUS], 1);
		run_until_set(m, A + INTFLAGS, F_RES, 400);
		check("event-started result = 2048", RES32(m), 2048);
		check("EVENT_TRIGGER stays armed", m->data[A + COMMAND] & 7, 4);
		ADC_CLEAR(m);
		avr_raise_irq(ainp0, 0);			/* falling edge: no start */
		check("falling event edge does not start", m->data[A + STATUS], 0);
		cpu_write(m, A + COMMAND, (1 << 4) | 0);		/* START = STOP */
		avr_raise_irq(ainp0, 2000);
		check("events ignored unless START=EVENT_TRIGGER", m->data[A + STATUS], 0);
	}

	printf("== AVR DD (AVR32DD14) core ==\n");
	{
		avr_t *m = avr_make_mcu_by_name("avr32dd14");
		if (!m) { printf("cannot make avr32dd14 core\n"); return 2; }
		m->log = LOG_ERROR;
		avr_init(m);
		avr_reset(m);
		/* DS40002413B 8.2, 9.1-9.3: memories, signature (Table 7-6 device ID),
		 * peripheral addresses and the 4-byte JMP vectors. */
		check("DD14 is MODERN", !!(m->arch.flags & AVR_ARCH_F_MODERN), 1);
		check("DD14 signature",
			  (m->signature[0] << 16) | (m->signature[1] << 8) | m->signature[2],
			  0x1e953b);
		check("DD14 flashend", m->flashend, 0x7fff);
		check("DD14 ramend", m->ramend, 0x7fff);
		check("DD14 e2end", m->e2end, 0x14ff);
		check("DD14 vector_size", m->vector_size, 4);
		check("DD14 flashmap 0x8000", m->arch.flashmap_start, 0x8000);
		check("DD14 PORTA", HAS_W(m, 0x0400 + 0x01), 1);
		check("DD14 PORTC", HAS_W(m, 0x0440 + 0x01), 1);
		check("DD14 PORTD", HAS_W(m, 0x0460 + 0x01), 1);
		check("DD14 PORTF", HAS_W(m, 0x04a0 + 0x01), 1);
		check("DD14 no PORTB", HAS_W(m, 0x0420 + 0x01), 0);
		check("DD14 USART1 @0x820", HAS_W(m, 0x0822), 1);
		check("DD14 TWI0 @0x900", HAS_W(m, 0x0904), 1);
		check("DD14 SPI0 @0x940", HAS_W(m, 0x0941), 1);
		check("DD14 TCB1 @0xB10", HAS_W(m, 0x0b10), 1);
		check("DD14 BOD @0xA0", HAS_W(m, 0x00a0), 1);

		/* SYSCFG / SIGROW / FUSE / LOCK (8.6, 8.8, 8.9, 9.3). */
		check("SIGROW.DEVICEID0", cpu_read(m, 0x1100), 0x1e);
		check("SIGROW.DEVICEID1", cpu_read(m, 0x1101), 0x95);
		check("SIGROW.DEVICEID2", cpu_read(m, 0x1102), 0x3b);
		check("FUSE.SYSCFG0 default 0xD0", cpu_read(m, 0x1050 + 5), 0xd0);
		check("FUSE.SYSCFG1 default 0x08", cpu_read(m, 0x1050 + 6), 0x08);
		check("FUSE.BOOTSIZE default 0", cpu_read(m, 0x1050 + 8), 0);
		check("LOCK.KEY0 unlocked", cpu_read(m, 0x1040), 0x5c);
		cpu_write(m, 0x1100, 0x55);
		check("SIGROW is read-only", cpu_read(m, 0x1100), 0x1e);

		printf("== AVR DD CLKCTRL ==\n");
		/* 12.3.3 / 12.5.7: OSCHF 4 MHz after reset, FRQSEL = 0x3. */
		check("reset CLK = 4 MHz", m->frequency, 4000000);
		check("OSCHFCTRLA reset 0x0C", m->data[0x68], 0x0c);
		check("MCLKSTATUS OSCHFS|OSC32KS", m->data[0x65], 0x06);
		cpu_write(m, 0x68, 0x24);			/* FRQSEL = 0x9 (24 MHz), no CCP */
		check("OSCHFCTRLA needs CCP", m->data[0x68], 0x0c);
		avr_ccp_write(m, AVR_CCP_IOREG);
		cpu_write(m, 0x68, 0x24);
		check("FRQSEL 24 MHz", m->frequency, 24000000);
		avr_ccp_write(m, AVR_CCP_IOREG);
		cpu_write(m, 0x61, (0x8 << 1) | 1);		/* PDIV = 6X, PEN */
		check("PDIV 6X -> 4 MHz", m->frequency, 4000000);
		avr_ccp_write(m, AVR_CCP_IOREG);
		cpu_write(m, 0x61, (0xc << 1) | 1);		/* PDIV = 48X */
		check("PDIV 48X -> 500 kHz", m->frequency, 500000);
		avr_ccp_write(m, AVR_CCP_IOREG);
		cpu_write(m, 0x61, 0);
		check("prescaler off -> 24 MHz", m->frequency, 24000000);
		/* 12.3.2: a switch to a disabled XOSC32K stays pending (SOSC set). */
		avr_ccp_write(m, AVR_CCP_IOREG);
		cpu_write(m, 0x60, 0x02);			/* CLKSEL = XOSC32K */
		check("pending switch sets SOSC", m->data[0x65] & 0x01, 1);
		check("pending switch keeps clock", m->frequency, 24000000);
		avr_ccp_write(m, AVR_CCP_IOREG);
		cpu_write(m, 0x7c, 0x01);			/* XOSC32KCTRLA.ENABLE */
		check("enabled XOSC32K completes switch", m->data[0x65] & 0x01, 0);
		check("XOSC32KS set", (m->data[0x65] >> 3) & 1, 1);
		check("CLK = 32.768 kHz", m->frequency, 32768);
		avr_ccp_write(m, AVR_CCP_IOREG);
		cpu_write(m, 0x80, 0x03);			/* XOSCHFCTRLA: SELHF | ENABLE */
		avr_ccp_write(m, AVR_CCP_IOREG);
		cpu_write(m, 0x60, 0x03);			/* CLKSEL = EXTCLK */
		check("EXTCLK completes switch", m->data[0x65] & 0x01, 0);
		check("EXTS set", (m->data[0x65] >> 4) & 1, 1);
		avr_ccp_write(m, AVR_CCP_IOREG);
		cpu_write(m, 0x60, 0x02);
		cpu_write(m, 0x69, 0x20);			/* OSCHFTUNE: bit 5 mirrors to 7:6 */
		check("OSCHFTUNE sign extends", m->data[0x69], 0xe0);
		cpu_write(m, 0x65, 0xff);
		check("MCLKSTATUS read-only", m->data[0x65] & 0xc0, 0);
		avr_reset(m);
		check("reset restores 4 MHz", m->frequency, 4000000);
		m->fuse[2] = 0x01;				/* FUSE.OSCCFG.CLKSEL = OSC32K */
		avr_reset(m);
		check("OSCCFG fuse selects OSC32K", m->frequency, 32768);
		m->fuse[2] = 0x00;
		avr_reset(m);

		printf("== AVR DD PORT multi-pin configuration ==\n");
		/* 18.3.2.4: PINCONFIG is mirrored; UPD copies, SET ORs, CLR clears. */
		cpu_write(m, 0x400 + 0x0b, 0x08 | 0x03);	/* PULLUPEN | ISC=FALLING */
		check("PINCONFIG mirrored to PORTC", m->data[0x440 + 0x0b], 0x0b);
		check("PINCONFIG mirrored to PORTF", m->data[0x4a0 + 0x0b], 0x0b);
		cpu_write(m, 0x440 + 0x0c, 0x0a);		/* PINCTRLUPD pins 1,3 */
		check("UPD copies to PIN1CTRL", m->data[0x440 + 0x11], 0x0b);
		check("UPD copies to PIN3CTRL", m->data[0x440 + 0x13], 0x0b);
		check("UPD leaves PIN0CTRL", m->data[0x440 + 0x10], 0x00);
		check("UPD reads zero", cpu_read(m, 0x440 + 0x0c), 0);
		cpu_write(m, 0x440 + 0x0b, 0x80);		/* PINCONFIG = INVEN */
		cpu_write(m, 0x440 + 0x0d, 0x01);		/* SET pin 0 */
		check("SET ORs INVEN into PIN0CTRL", m->data[0x440 + 0x10], 0x80);
		cpu_write(m, 0x440 + 0x0b, 0x08);		/* PINCONFIG = PULLUPEN */
		cpu_write(m, 0x440 + 0x0e, 0x02);		/* CLR pin 1 */
		check("CLR clears PULLUPEN in PIN1CTRL", m->data[0x440 + 0x11], 0x03);
		cpu_write(m, 0x440 + 0x0b, 0xff);
		check("PINCONFIG reserved bits read 0", m->data[0x440 + 0x0b], 0xcf);

		printf("== AVR DD NVMCTRL ==\n");
		{
#define N 0x1000
#define EE 0x1400
#define NVM_RUN() do { for (int _i = 0; _i < 400; _i++) { avr_run(m); \
				if (m->pc >= 0x1000) m->pc = 0; } } while (0)
			avr_reset(m);
			check("CTRLB reset FLMAP=3", m->data[N + 1], 0x30);
			check("EEPROM erased", m->data[EE + 3], 0xff);
			cpu_write(m, EE + 3, 0x11);			/* no command armed */
			check("store without command ignored", m->data[EE + 3], 0xff);
			avr_ccp_write(m, AVR_CCP_SPM);
			cpu_write(m, N + 0, 0x12);			/* EEWR */
			cpu_write(m, EE + 3, 0x11);
			check("EEWR stores", m->data[EE + 3], 0x11);
			check("EEBUSY after write", m->data[N + 2] & 0x02, 2);
			check("EEREADY cleared while busy", m->data[N + 4] & 1, 0);
			check("ADDR holds last EEPROM address", m->data[N + 8], 3);
			check("DATA holds last EEPROM data", m->data[N + 6], 0x11);
			/* 11.5.1: a command while busy -> command collision. */
			avr_ccp_write(m, AVR_CCP_SPM);
			cpu_write(m, N + 0, 0x18);
			check("collision error", (m->data[N + 2] >> 4) & 7, 3);
			NVM_RUN();
			check("EEBUSY clears", m->data[N + 2] & 0x02, 0);
			check("EEREADY set when idle", m->data[N + 4] & 1, 1);
			cpu_write(m, N + 2, 0x00);			/* ERROR cleared by writing 0 */
			check("ERROR cleared by writing 0", m->data[N + 2] & 0x70, 0);
			cpu_write(m, N + 0, 0x00);
			check("CTRLA needs CCP", m->data[N + 0], 0x12);
			avr_ccp_write(m, AVR_CCP_SPM);
			cpu_write(m, N + 0, 0x7f);			/* not a defined command */
			check("invalid command error", (m->data[N + 2] >> 4) & 7, 1);
			check("invalid command not stored", m->data[N + 0], 0x12);
			cpu_write(m, N + 2, 0x00);
			avr_ccp_write(m, AVR_CCP_SPM);
			cpu_write(m, N + 0, 0x19);			/* EEMBER2 */
			cpu_write(m, EE + 3, 0x00);			/* erases bytes 2..3 */
			check("EEMBER2 erased byte 3", m->data[EE + 3], 0xff);
			NVM_RUN();
			avr_ccp_write(m, AVR_CCP_SPM);
			cpu_write(m, N + 0, 0x13);			/* EEERWR */
			for (int i = 0; i < 8; i++) { cpu_write(m, EE + 8 + i, 0x40 + i); NVM_RUN(); }
			check("EEERWR byte 7", m->data[EE + 15], 0x47);
			avr_ccp_write(m, AVR_CCP_SPM);
			cpu_write(m, N + 0, 0x1b);			/* EEMBER8 */
			NVM_RUN();
			cpu_write(m, EE + 11, 0x00);		/* erases 8..15 */
			check("EEMBER8 erased first byte", m->data[EE + 8], 0xff);
			check("EEMBER8 erased last byte", m->data[EE + 15], 0xff);
			NVM_RUN();
			avr_ccp_write(m, AVR_CCP_SPM);
			cpu_write(m, N + 0, 0x18);			/* EEBER */
			cpu_write(m, EE + 3, 0x00);
			check("EEBER reads 0xFF", m->data[EE + 3], 0xff);
			NVM_RUN();
			avr_ccp_write(m, AVR_CCP_SPM);
			cpu_write(m, N + 0, 0x12);
			cpu_write(m, EE + 200, 0x77);
			NVM_RUN();
			avr_reset(m);
			check("EEPROM persists across reset", m->data[EE + 200], 0x77);
			avr_ccp_write(m, AVR_CCP_SPM);
			cpu_write(m, N + 0, 0x30);			/* EECHER */
			check("EECHER erases EEPROM", m->data[EE + 200], 0xff);
			check("EECHER busy", m->data[N + 2] & 0x02, 2);
			NVM_RUN();
			/* USERROW: normal flash-style FLWR / FLPER (8.7, 11.3.1.4). */
			avr_ccp_write(m, AVR_CCP_SPM);
			cpu_write(m, N + 0, 0x02);
			cpu_write(m, 0x1080 + 4, 0x5a);
			check("USERROW FLWR", m->data[0x1080 + 4], 0x5a);
			NVM_RUN();
			avr_ccp_write(m, AVR_CCP_SPM);
			cpu_write(m, N + 0, 0x01);			/* NOOP between commands */
			avr_ccp_write(m, AVR_CCP_SPM);
			cpu_write(m, N + 0, 0x08);
			cpu_write(m, 0x1080 + 9, 0x00);
			check("USERROW FLPER erases the row", m->data[0x1080 + 4], 0xff);
			NVM_RUN();
			avr_ccp_write(m, AVR_CCP_SPM);
			cpu_write(m, N + 0, 0x00);

			/* Flash sections (11.3.1.1 / Table 11-1): with the default fuses the
			 * whole flash is BOOT and can never be written by code. */
			m->pc = 0;
			avr_ccp_write(m, AVR_CCP_SPM);
			cpu_write(m, N + 0, 0x02);			/* FLWR */
			m->flashmap_write(m, 0x8000 + 0x1000, 0x00, m->flashmap_write_param);
			check("default fuses: BOOT write refused", (m->data[N + 2] >> 4) & 7, 2);
			check("flash untouched", m->flash[0x1000], 0xff);
			cpu_write(m, N + 2, 0x00);
			/* BOOTSIZE = 4 (2 KB), CODESIZE = 8 (4 KB): BOOT 0-0x7FF, APPCODE
			 * 0x800-0xFFF, APPDATA 0x1000-. */
			m->fuse[8] = 4; m->fuse[7] = 8;
			memset(m->flash + 0x800, 0xaa, 0x2000);
			m->pc = 0x0100;						/* executing from BOOT */
			avr_ccp_write(m, AVR_CCP_SPM);
			cpu_write(m, N + 0, 0x00);
			avr_ccp_write(m, AVR_CCP_SPM);
			cpu_write(m, N + 0, 0x02);
			m->flashmap_write(m, 0x8000 + 0x0900, 0x12, m->flashmap_write_param);
			check("BOOT may write APPCODE", m->flash[0x900], 0x12);
			NVM_RUN();
			m->flashmap_write(m, 0x8000 + 0x0100, 0x34, m->flashmap_write_param);
			check("BOOT may not write BOOT", (m->data[N + 2] >> 4) & 7, 2);
			check("BOOT unchanged", m->flash[0x100] == 0x34, 0);
			cpu_write(m, N + 2, 0x00);
			NVM_RUN();
			m->flashmap_write(m, 0x8000 + 0x1100, 0x56, m->flashmap_write_param);
			check("BOOT may write APPDATA", m->flash[0x1100], 0x56);
			NVM_RUN();
			m->pc = 0x0a00;						/* executing from APPCODE */
			m->flashmap_write(m, 0x8000 + 0x0a10, 0x78, m->flashmap_write_param);
			check("APPCODE may not write APPCODE", (m->data[N + 2] >> 4) & 7, 2);
			cpu_write(m, N + 2, 0x00);
			NVM_RUN();
			m->flashmap_write(m, 0x8000 + 0x1200, 0x9a, m->flashmap_write_param);
			check("APPCODE may write APPDATA", m->flash[0x1200], 0x9a);
			NVM_RUN();
			m->pc = 0x1100;						/* executing from APPDATA */
			m->flashmap_write(m, 0x8000 + 0x1300, 0xbc, m->flashmap_write_param);
			check("APPDATA may not write APPDATA", (m->data[N + 2] >> 4) & 7, 2);
			cpu_write(m, N + 2, 0x00);
			/* Page erase / multi-page erase from BOOT. */
			m->pc = 0x0100;
			avr_ccp_write(m, AVR_CCP_SPM);
			cpu_write(m, N + 0, 0x01);
			avr_ccp_write(m, AVR_CCP_SPM);
			cpu_write(m, N + 0, 0x08);			/* FLPER */
			NVM_RUN();
			m->flashmap_write(m, 0x8000 + 0x1010, 0x00, m->flashmap_write_param);
			check("FLPER erases the page", m->flash[0x1000], 0xff);
			check("FLPER erases the page end", m->flash[0x11ff], 0xff);
			check("FLPER leaves the next page", m->flash[0x1200], 0x9a);
			NVM_RUN();
			avr_ccp_write(m, AVR_CCP_SPM);
			cpu_write(m, N + 0, 0x01);
			avr_ccp_write(m, AVR_CCP_SPM);
			cpu_write(m, N + 0, 0x0a);			/* FLMPER4 */
			m->flashmap_write(m, 0x8000 + 0x1600, 0x00, m->flashmap_write_param);
			check("FLMPER4 first page", m->flash[0x1000 + 0x000], 0xff);
			check("FLMPER4 erases 4 pages", m->flash[0x1000 + 0x7ff], 0xff);
			check("FLMPER4 stops after 4 pages", m->flash[0x1800], 0xaa);
			NVM_RUN();
			/* CTRLB write-protect bits are CCP protected and sticky. */
			avr_ccp_write(m, AVR_CCP_IOREG);
			cpu_write(m, N + 1, 0x30 | 0x01);		/* APPCODEWP */
			check("APPCODEWP set", m->data[N + 1] & 1, 1);
			avr_ccp_write(m, AVR_CCP_IOREG);
			cpu_write(m, N + 1, 0x30);
			check("APPCODEWP only cleared by reset", m->data[N + 1] & 1, 1);
			avr_ccp_write(m, AVR_CCP_SPM);
			cpu_write(m, N + 0, 0x01);
			avr_ccp_write(m, AVR_CCP_SPM);
			cpu_write(m, N + 0, 0x02);
			m->flashmap_write(m, 0x8000 + 0x0b00, 0x01, m->flashmap_write_param);
			check("APPCODEWP blocks APPCODE", (m->data[N + 2] >> 4) & 7, 2);
			cpu_write(m, N + 2, 0x00);
			cpu_write(m, N + 1, 0x00);			/* FLMAP is writable without CCP */
			check("FLMAP not under CCP", m->data[N + 1] & 0x30, 0x00);
			avr_ccp_write(m, AVR_CCP_IOREG);
			cpu_write(m, N + 1, 0x80);			/* FLMAPLOCK */
			cpu_write(m, N + 1, 0x30);
			check("FLMAPLOCK freezes FLMAP", m->data[N + 1] & 0x30, 0x00);
			/* A multi-page erase touching a protected page erases nothing and
			 * starts no operation. FLMPER4 at 0x600 covers pages 0x600-0xdff
			 * (BOOT 0-0x7ff): executing from BOOT, BOOT pages are refused. */
			cpu_write(m, N + 1, 0x00);
			m->pc = 0x0100;
			avr_reset(m);
			m->fuse[8] = 4; m->fuse[7] = 8;
			memset(m->flash + 0x600, 0xaa, 0x800);
			avr_ccp_write(m, AVR_CCP_SPM);
			cpu_write(m, N + 0, 0x0a);
			m->flashmap_write(m, 0x8000 + 0x0600, 0x00, m->flashmap_write_param);
			check("protected FLMPER4 sets WRITEPROTECT", (m->data[N + 2] >> 4) & 7, 2);
			check("protected FLMPER4 erases nothing", m->flash[0x900], 0xaa);
			check("protected FLMPER4 starts no op", m->data[N + 2] & 1, 0);
			m->fuse[8] = 0; m->fuse[7] = 0;
			m->fuse[8] = 0; m->fuse[7] = 0;
			/* EEREADY is a level interrupt: enabling it while idle pends NVMCTRL_EE
			 * (vector 35, 11.5.4). */
			avr_reset(m);
			check("no interrupt pending after reset", avr_has_pending_interrupts(m), 0);
			cpu_write(m, N + 3, 0x01);
			check("EEREADY enable pends the vector", avr_has_pending_interrupts(m), 1);
			cpu_write(m, N + 3, 0x00);
			check("EEREADY disable unpends the vector", avr_has_pending_interrupts(m), 0);
		}
#undef N
#undef EE
#undef NVM_RUN
	}

	printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
			failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
