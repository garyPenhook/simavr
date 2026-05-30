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

/* Captures the DAC output (millivolts, full width). */
static uint32_t g_dac_out;
static void dac_capture_hook(struct avr_irq_t *irq, uint32_t value, void *param)
{
	(void)irq; (void)param;
	g_dac_out = value;
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

		avr_t *m = avr_make_mcu_by_name("attiny3217");
		if (!m) { printf("cannot make attiny3217 core\n"); return 2; }
		m->log = LOG_ERROR;
		avr_init(m);

		check("erased EEPROM reads 0xFF", cpu_read(m, EE + 5), 0xff);

		/* A write loads the page buffer but does NOT commit. */
		cpu_write(m, EE + 5, 0xab);
		check("EEPROM unchanged before command", cpu_read(m, EE + 5), 0xff);

		/* Command ignored without an open CCP window. */
		m->arch.ccp_window = 0;
		cpu_write(m, NV + CTRLA, CMD_PAGEERASEWRITE);
		check("commit ignored without CCP", cpu_read(m, EE + 5), 0xff);

		/* With CCP, ERASEWRITE commits the buffered byte. */
		avr_ccp_write(m, AVR_CCP_IOREG);
		cpu_write(m, NV + CTRLA, CMD_PAGEERASEWRITE);
		check("ERASEWRITE commits byte", cpu_read(m, EE + 5), 0xab);
		check("EEREADY flag set after commit",
			  !!(m->data[NV + INTFLAGS] & EEREADY), 1);
		check("EEBUSY clear (instant)", !!(m->data[NV + STATUS] & 0x02), 0);

		/* Multiple bytes, in place. */
		cpu_write(m, EE + 0, 0x11);
		cpu_write(m, EE + 1, 0x22);
		avr_ccp_write(m, AVR_CCP_IOREG);
		cpu_write(m, NV + CTRLA, CMD_PAGEWRITE);
		check("byte 0 committed", cpu_read(m, EE + 0), 0x11);
		check("byte 1 committed", cpu_read(m, EE + 1), 0x22);
		check("untouched byte preserved", cpu_read(m, EE + 5), 0xab);

		/* PAGEBUFCLR aborts a pending buffer load. */
		cpu_write(m, EE + 0, 0x99);
		avr_ccp_write(m, AVR_CCP_IOREG);
		cpu_write(m, NV + CTRLA, CMD_PAGEBUFCLR);
		avr_ccp_write(m, AVR_CCP_IOREG);
		cpu_write(m, NV + CTRLA, CMD_PAGEWRITE);
		check("PAGEBUFCLR discarded the write", cpu_read(m, EE + 0), 0x11);

		/* EE interrupt fires when enabled. */
		cpu_write(m, NV + INTFLAGS, EEREADY);	/* clear */
		cpu_write(m, NV + INTCTRL, EEREADY);	/* enable */
		cpu_write(m, EE + 2, 0x33);
		avr_ccp_write(m, AVR_CCP_IOREG);
		cpu_write(m, NV + CTRLA, CMD_PAGEWRITE);
		check("EE interrupt pending", avr_has_pending_interrupts(m), 1);
		cpu_write(m, NV + INTFLAGS, EEREADY);	/* W1C */
		check("EEREADY cleared by W1C", !!(m->data[NV + INTFLAGS] & EEREADY), 0);

		/* EEERASE wipes the whole EEPROM. */
		avr_ccp_write(m, AVR_CCP_IOREG);
		cpu_write(m, NV + CTRLA, CMD_EEERASE);
		check("EEERASE wipes byte 0", cpu_read(m, EE + 0), 0xff);
		check("EEERASE wipes byte 5", cpu_read(m, EE + 5), 0xff);
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
	}

	printf("== modern DAC0 (sim_tiny3217) ==\n");
	{
		const avr_io_addr_t D = 0x6a0;
		enum { ENABLE = 0x01 };
		/* default vref = 1100 mV; out = DATA * 1100 / 256. */

		avr_t *m = avr_make_mcu_by_name("attiny3217");
		if (!m) { printf("cannot make attiny3217 core\n"); return 2; }
		m->log = LOG_ERROR;
		avr_init(m);
		memset(m->flash, 0, 0x2000);

		g_dac_out = 0xffffffff;
		avr_irq_register_notify(
			avr_io_getirq(m, AVR_IOCTL_DAC_GETIRQ('0'), AVR_DAC_IRQ_OUT),
			dac_capture_hook, NULL);

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

	printf("== modern CCL (sim_tiny3217) ==\n");
	{
		const avr_io_addr_t L = 0x1c0;
		enum { CTRLA = 0x00, L0CTRLA = 0x05, L0CTRLB = 0x06, L0CTRLC = 0x07,
			   TRUTH0 = 0x08, L1CTRLA = 0x09, L1CTRLB = 0x0a, L1CTRLC = 0x0b,
			   TRUTH1 = 0x0c };
		enum { CCL_EN = 0x01, LUT_EN = 0x01 };
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

		avr_t *m = avr_make_mcu_by_name("attiny3217");
		if (!m) { printf("cannot make attiny3217 core\n"); return 2; }
		m->log = LOG_ERROR;
		avr_init(m);

		/* Enabling completes instantly: OK set, never BUSY. */
		cpu_write(m, C + CTRLA, ENABLE);
		check("CRCSCAN OK after enable", !!(m->data[C + STATUS] & OK), 1);
		check("CRCSCAN never BUSY", !!(m->data[C + STATUS] & BUSY), 0);

		/* Disabling clears the result. */
		cpu_write(m, C + CTRLA, 0x00);
		check("CRCSCAN OK cleared when disabled", !!(m->data[C + STATUS] & OK), 0);

		/* RESET strobe clears the result and disables (and self-clears). */
		cpu_write(m, C + CTRLA, ENABLE);
		cpu_write(m, C + CTRLA, RESET);
		check("CRCSCAN OK cleared by RESET", !!(m->data[C + STATUS] & OK), 0);
		check("CRCSCAN RESET self-clears CTRLA", m->data[C + CTRLA], 0);

		/* NMIEN locks CTRLA: the scan cannot be disabled until reset. */
		cpu_write(m, C + CTRLA, ENABLE | NMIEN);
		check("CRCSCAN OK with NMIEN", !!(m->data[C + STATUS] & OK), 1);
		cpu_write(m, C + CTRLA, 0x00);	/* attempt to disable */
		check("CTRLA locked by NMIEN", m->data[C + CTRLA], ENABLE | NMIEN);
		check("CRCSCAN still OK (locked)", !!(m->data[C + STATUS] & OK), 1);
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

	printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
			failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
