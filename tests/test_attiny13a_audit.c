/*
	test_attiny13a_audit.c

	ATtiny13A (DS40002307A) peripheral-by-peripheral conformance checks.

	Host-only, like test_avrxt_engine.c: a real core is allocated, registers are
	poked the way the CPU does (through the io callbacks) and time is advanced by
	running NOPs. Every check names the datasheet section it comes from.

	  cc -I simavr/sim -I simavr/cores -o /tmp/t tests/test_attiny13a_audit.c \
	     -L simavr/obj-* -lsimavr -lelf -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sim_avr.h"
#include "sim_core.h"
#include "sim_host.h"
#include "sim_interrupts.h"
#include "avr_adc.h"
#include "avr_acomp.h"
#include "avr_eeprom.h"
#include "avr_flash.h"
#include "avr_ioport.h"

/* Data-space addresses: register summary, section 20, plus the 0x20 I/O offset. */
enum {
	SREG = 0x5f, GIMSK = 0x5b, GIFR = 0x5a, TIMSK0 = 0x59, TIFR0 = 0x58, SPMCSR = 0x57,
	OCR0A = 0x56, MCUCR = 0x55, MCUSR = 0x54, TCCR0B = 0x53, TCNT0 = 0x52, BODCR = 0x50,
	TCCR0A = 0x4f, OCR0B = 0x49, GTCCR = 0x48, WDTCR = 0x41, EEAR = 0x3e, EEDR = 0x3d,
	EECR = 0x3c, PORTB = 0x38, DDRB = 0x37, PINB = 0x36, PCMSK = 0x35, DIDR0 = 0x34,
	ACSR = 0x28, ADMUX = 0x27, ADCSRA = 0x26, ADCH = 0x25, ADCL = 0x24, ADCSRB = 0x23,
};

/* interrupt vector numbers, Table 9-1 */
enum { V_INT0 = 1, V_PCINT0, V_TIM0_OVF, V_EE_RDY, V_ANA_COMP, V_TIM0_COMPA, V_TIM0_COMPB, V_WDT, V_ADC };

#define F_CPU 1200000UL

static int failures;

static void check(const char *what, long got, long want)
{
	if (got != want) {
		printf("  FAIL: %-58s got %ld, want %ld\n", what, got, want);
		failures++;
	} else {
		printf("  ok:   %-58s = %ld\n", what, got);
	}
}

static void cw(avr_t *avr, uint16_t addr, uint8_t v)
{
	avr_io_addr_t io = AVR_DATA_TO_IO(addr);
	if (avr->io[io].w.c)
		avr->io[io].w.c(avr, addr, v, avr->io[io].w.param);
	else
		avr_core_watch_write(avr, addr, v);
}

static uint8_t cr(avr_t *avr, uint16_t addr)
{
	avr_io_addr_t io = AVR_DATA_TO_IO(addr);
	if (avr->io[io].r.c)
		return avr->io[io].r.c(avr, addr, avr->io[io].r.param);
	return avr->data[addr];
}

/* Run NOPs for the given number of cycles; the pc is kept inside the NOP-filled flash. */
static void run(avr_t *avr, long cycles)
{
	avr_cycle_count_t end = avr->cycle + cycles;
	while (avr->cycle < end) {
		if (avr->state == cpu_Running)
			avr->pc = 0x100;
		avr_run(avr);
	}
}

static void nosleep(avr_t *avr, avr_cycle_count_t how_long) { (void)avr; (void)how_long; }

/* A fresh core with the timers idle for a while (a timer started at cycle 0 has no phase to speak of). */
static avr_t *mk(void)
{
	avr_t *avr = avr_make_mcu_by_name("attiny13a");
	if (!avr) { printf("cannot make core\n"); exit(2); }
	avr->log = LOG_ERROR;
	avr->frequency = F_CPU;
	avr->vcc = avr->avcc = avr->aref = 5000;
	avr_init(avr);
	avr->sleep = nosleep;
	avr->data[avr->arch.sp_addr] = 0x9f;
	run(avr, 1000);
	return avr;
}

/* Execute one hand-assembled instruction at the pc. */
static void exec(avr_t *avr, uint16_t op)
{
	avr->flash[0x100] = op & 0xff;
	avr->flash[0x101] = op >> 8;
	avr->pc = 0x100;
	avr->pc = avr_run_one(avr);
	avr->flash[0x100] = avr->flash[0x101] = 0;	/* back to the NOP that run() executes */
}

static uint8_t g_pin[2];
static void rec(struct avr_irq_t *irq, uint32_t v, void *param)
{
	((uint8_t *)param)[irq->irq] = v & 1;
}

static avr_irq_t *pin_irq(avr_t *avr, int bit)
{
	return avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ('B'), bit);
}

/* Like mk() but with PB0/PB1 (OC0A/OC0B) outputs whose level is recorded in g_pin[]. */
static avr_t *mk_pins(void)
{
	avr_t *avr = mk();
	memset(g_pin, 0, sizeof(g_pin));
	avr_irq_register_notify(pin_irq(avr, 0), rec, g_pin);
	avr_irq_register_notify(pin_irq(avr, 1), rec, g_pin);
	cw(avr, DDRB, 0x03);
	return avr;
}

static uint8_t ee_byte(avr_t *avr, int off)
{
	avr_eeprom_desc_t d = { .ee = NULL, .offset = off, .size = 1 };
	avr_ioctl(avr, AVR_IOCTL_EEPROM_GET, &d);
	return *d.ee;
}

static void eecr_write(avr_t *avr, uint8_t pm, int addr, uint8_t data)
{
	cw(avr, EEAR, addr);
	cw(avr, EEDR, data);
	cw(avr, EECR, pm | 0x04);	/* EEMPE */
	cw(avr, EECR, pm | 0x06);	/* EEPE within four cycles */
}

/* ---------------------------------------------------------------------------------------- */

static void test_cores(void)
{
	printf("== core names (the GUI/host picker lists aliases)\n");
	int a13 = 0, a13a = 0;
	for (int i = 0; i < avr_host_core_count(); i++) {
		const char *n = avr_host_core_name(i);
		a13 += !strcmp(n, "attiny13");
		a13a += !strcmp(n, "attiny13a");
	}
	check("attiny13 is listed", a13, 1);
	check("attiny13a is listed", a13a, 1);
}

static void test_registers(void)
{
	printf("== reserved bits read as zero (section 20 register summary)\n");
	avr_t *avr = mk();
	struct { const char *n; uint16_t reg; uint8_t want; } t[] = {
		{ "PORTB   5:0", PORTB, 0x3f }, { "DDRB    5:0", DDRB, 0x3f }, { "PCMSK   5:0", PCMSK, 0x3f },
		{ "GIMSK   INT0,PCIE", GIMSK, 0x60 }, { "MCUCR   no bits 7,2", MCUCR, 0x7b },
		{ "TIMSK0  OCIE0B,A TOIE0", TIMSK0, 0x0e }, { "TCCR0A  no bits 3:2", TCCR0A, 0xf3 },
		{ "TCCR0B  FOC reads 0", TCCR0B, 0x0f }, { "EECR    5:0", EECR, 0x3f },
		{ "EEAR    5:0 (64 bytes)", EEAR, 0x3f }, { "ADMUX   REFS0 ADLAR MUX1:0", ADMUX, 0x63 },
		{ "ADCSRB  ACME ADTS", ADCSRB, 0x47 }, { "DIDR0   5:0", DIDR0, 0x3f }, { "GTCCR   TSM PSR10", GTCCR, 0x81 },
	};
	for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
		avr_t *a = mk();
		cw(a, t[i].reg, 0xff);
		/* EERE and EEPE are strobe/busy flags and EEMPE times out; TSM can not be left set in a loop */
		uint8_t ignore = t[i].reg == EECR ? 0x07 : t[i].reg == GTCCR ? 0x80 : 0;
		check(t[i].n, cr(a, t[i].reg) & ~ignore, t[i].want & ~ignore);
	}

	printf("== MCUSR (8.5.1): flags are cleared by writing zero, never set by writing one\n");
	check("PORF set after power-on", cr(avr, MCUSR), 0x01);
	cw(avr, MCUSR, 0xff);
	check("writing 1s sets nothing and clears nothing", cr(avr, MCUSR), 0x01);
	cw(avr, MCUSR, 0x00);
	check("writing 0 clears PORF", cr(avr, MCUSR), 0x00);
	cw(avr, MCUSR, 0xff);
	check("writing 1s afterwards does not set it again", cr(avr, MCUSR), 0x00);

	printf("== BODCR (7.5.1): BODS needs the BODSE timed sequence, then clears itself\n");
	avr = mk();
	cw(avr, BODCR, 0x02);
	check("BODS alone is refused", cr(avr, BODCR), 0x00);
	cw(avr, BODCR, 0x03);
	check("BODS+BODSE: both set", cr(avr, BODCR), 0x03);
	run(avr, 1);
	cw(avr, BODCR, 0x02);
	check("BODS=1, BODSE=0 inside the window: BODS active", cr(avr, BODCR), 0x02);
	run(avr, 4);
	check("BODS clears itself after three cycles", cr(avr, BODCR), 0x00);
	avr = mk();
	cw(avr, BODCR, 0x03);
	run(avr, 5);
	check("BODS does not outlive three cycles even if the sequence is abandoned", cr(avr, BODCR), 0x00);
	avr = mk();
	cw(avr, BODCR, 0x01);
	run(avr, 5);
	cw(avr, BODCR, 0x02);
	check("BODSE expired after four cycles: BODS refused", cr(avr, BODCR), 0x00);
}

static void test_watchdog(void)
{
	printf("== Watchdog (8.4, 8.5.2)\n");
	/* WDP = 0: 2K cycles of the 128 kHz oscillator = 16 ms = 19200 cycles at 1.2 MHz */
	avr_t *avr = mk();
	cw(avr, WDTCR, 0x40);			/* interrupt mode */
	run(avr, 19000);
	check("no WDTIF before the time-out", cr(avr, WDTCR) & 0x80, 0);
	run(avr, 400);
	check("WDTIF set at the time-out", cr(avr, WDTCR) & 0x80, 0x80);
	cw(avr, WDTCR, 0x40);
	check("writing a zero to WDTIF does not clear it", cr(avr, WDTCR) & 0x80, 0x80);
	cw(avr, WDTCR, 0xc0);
	check("writing a one to WDTIF clears it", cr(avr, WDTCR) & 0x80, 0);
	check("WDTIE untouched by the W1C", cr(avr, WDTCR) & 0x40, 0x40);

	avr = mk();
	cw(avr, WDTCR, 0x18);			/* WDCE | WDE */
	check("WDCE is set", cr(avr, WDTCR) & 0x10, 0x10);
	run(avr, 5);
	check("WDCE is cleared by hardware after four cycles", cr(avr, WDTCR) & 0x10, 0);
	cw(avr, WDTCR, 0x08);
	check("WDE can be set at any time", cr(avr, WDTCR) & 0x08, 0x08);
	cw(avr, WDTCR, 0x00);
	check("WDE can not be cleared outside the timed sequence", cr(avr, WDTCR) & 0x08, 0x08);
	cw(avr, WDTCR, 0x18);
	cw(avr, WDTCR, 0x00);
	check("WDE cleared by the WDCE sequence", cr(avr, WDTCR) & 0x08, 0);

	/* Interrupt and System Reset mode: "If the interrupt is not executed before the next
	 * time-out, a System Reset will be applied." */
	avr = mk();
	cw(avr, WDTCR, 0x18);
	cw(avr, WDTCR, 0x48);			/* WDE | WDTIE */
	run(avr, 19400);
	check("first time-out only raises the interrupt", cr(avr, WDTCR) & 0x80, 0x80);
	check("... and no reset yet", cr(avr, MCUSR) & 0x08, 0);
	run(avr, 19400);
	check("second time-out, interrupt not executed: WDRF set", cr(avr, MCUSR) & 0x08, 0x08);

	/* Same mode, but the interrupt is executed: WDTIE and WDTIF are cleared by hardware. */
	avr = mk();
	avr->sreg[S_I] = 1;
	cw(avr, WDTCR, 0x18);
	cw(avr, WDTCR, 0x48);
	run(avr, 19400);
	check("interrupt executed: WDTIF cleared", cr(avr, WDTCR) & 0x80, 0);
	check("interrupt executed: WDTIE cleared (reset mode now)", cr(avr, WDTCR) & 0x40, 0);
	run(avr, 19400);
	check("... so the next time-out resets", cr(avr, MCUSR) & 0x08, 0x08);

	/* clearing WDTIF by software must not clear WDTIE in Interrupt and System Reset mode */
	avr = mk();
	cw(avr, WDTCR, 0x18);
	cw(avr, WDTCR, 0x48);
	run(avr, 19400);
	cw(avr, WDTCR, 0xc8);
	check("W1C of WDTIF clears the flag", cr(avr, WDTCR) & 0x80, 0);
	check("... and leaves WDTIE set", cr(avr, WDTCR) & 0x40, 0x40);

	/* WDTON: the timed sequence that would disable the watchdog must not leave it unarmed */
	avr = avr_make_mcu_by_name("attiny13a");
	avr->log = LOG_ERROR;
	avr->frequency = F_CPU;
	avr->fuse[0] &= ~(1 << 5);
	avr_init(avr);
	avr->sleep = nosleep;
	run(avr, 1000);
	cw(avr, WDTCR, 0x18);
	cw(avr, WDTCR, 0x00);
	run(avr, 19400);
	check("WDTON: disable attempt, the watchdog still resets", cr(avr, MCUSR) & 0x08, 0x08);

	/* WDTON fuse (Table 17-4, bit 5 of the low byte), programmed = 0 */
	avr = avr_make_mcu_by_name("attiny13a");
	avr->log = LOG_ERROR;
	avr->frequency = F_CPU;
	avr->fuse[0] &= ~(1 << 5);
	avr_init(avr);
	avr->sleep = nosleep;
	run(avr, 1000);
	check("WDTON programmed: WDE reads 1 out of reset", cr(avr, WDTCR) & 0x08, 0x08);
	cw(avr, WDTCR, 0x18);
	cw(avr, WDTCR, 0x40);			/* try WDE=0, WDTIE=1 */
	check("WDTON: WDE locked to 1", cr(avr, WDTCR) & 0x08, 0x08);
	check("WDTON: WDTIE locked to 0", cr(avr, WDTCR) & 0x40, 0);
}

static void test_timer_basics(void)
{
	printf("== Timer/Counter0: flags and modes (11.7, Table 11-8)\n");
	avr_t *avr = mk();
	cw(avr, TCCR0B, 1);
	run(avr, 255);
	check("normal: TCNT0 = 255", cr(avr, TCNT0), 255);
	check("normal: no TOV0 yet", cr(avr, TIFR0) & 2, 0);
	run(avr, 1);
	check("normal: TOV0 set when the counter wraps (MAX -> BOTTOM)", cr(avr, TIFR0) & 2, 2);
	cw(avr, TIFR0, 0x02);
	check("TOV0 cleared by writing a one", cr(avr, TIFR0) & 2, 0);

	avr = mk();
	cw(avr, OCR0A, 9);
	cw(avr, TCCR0A, 0x02);			/* CTC */
	cw(avr, TCCR0B, 1);
	run(avr, 10);
	check("CTC: OCF0A set when TCNT0 reaches TOP", cr(avr, TIFR0) & 4, 4);
	check("CTC: TCNT0 cleared at TOP", cr(avr, TCNT0), 0);
	check("CTC: TOV0 NOT set (Table 11-8: set on MAX)", cr(avr, TIFR0) & 2, 0);
	cw(avr, OCR0A, 0xff);
	cw(avr, TIFR0, 0x06);
	run(avr, 600);
	check("CTC with TOP = MAX: TOV0 set", cr(avr, TIFR0) & 2, 2);

	printf("== Fast PWM, OCR0A as TOP (mode 7)\n");
	avr = mk();
	cw(avr, OCR0A, 9);
	cw(avr, TCCR0A, 0x03);
	cw(avr, TCCR0B, 0x09);
	run(avr, 9);
	check("mode 7: TCNT0 = TOP", cr(avr, TCNT0), 9);
	run(avr, 1);
	check("mode 7: period is TOP+1 clocks", cr(avr, TCNT0), 0);
	check("mode 7: TOV0 set on TOP", cr(avr, TIFR0) & 2, 2);
	check("mode 7: OCF0A set on TOP", cr(avr, TIFR0) & 4, 4);

	printf("== Phase correct PWM: period 510, TOV0 at BOTTOM, OCR0x latched at TOP\n");
	avr = mk();
	cw(avr, TCCR0A, 0x01);
	cw(avr, TCCR0B, 1);
	run(avr, 255);
	check("PC: TCNT0 reaches TOP after 255 clocks", cr(avr, TCNT0), 255);
	check("PC: no TOV0 at TOP", cr(avr, TIFR0) & 2, 0);
	run(avr, 100);
	check("PC: counts down after TOP", cr(avr, TCNT0), 155);
	run(avr, 155);
	check("PC: BOTTOM after 510 clocks", cr(avr, TCNT0), 0);
	check("PC: TOV0 set at BOTTOM", cr(avr, TIFR0) & 2, 2);

	avr = mk();
	cw(avr, OCR0A, 20);
	cw(avr, TCCR0A, 0x01);
	cw(avr, TCCR0B, 0x09);			/* mode 5, TOP = OCR0A */
	run(avr, 20);
	check("mode 5: TCNT0 = TOP = OCR0A", cr(avr, TCNT0), 20);
	run(avr, 20);
	check("mode 5: period 2*TOP", cr(avr, TCNT0), 0);
	check("mode 5: TOV0 at BOTTOM", cr(avr, TIFR0) & 2, 2);

	printf("== Compare match blocking by a TCNT0 write (11.9.3)\n");
	avr = mk();
	cw(avr, OCR0A, 50);
	cw(avr, TCCR0B, 1);
	run(avr, 10);
	cw(avr, TCNT0, 50);
	run(avr, 3);
	check("TCNT0 := OCR0A blocks the match", cr(avr, TIFR0) & 4, 0);
	avr = mk();
	cw(avr, OCR0A, 50);
	cw(avr, TCCR0B, 1);
	run(avr, 60);
	check("without the write the match is flagged", cr(avr, TIFR0) & 4, 4);

	printf("== GTCCR (12.4.1): PSR10 clears itself, TSM halts the counter\n");
	avr = mk();
	cw(avr, TCCR0B, 1);
	run(avr, 20);
	cw(avr, GTCCR, 0x81);
	run(avr, 100);
	check("TSM: counter halted", cr(avr, TCNT0), 20);
	cw(avr, GTCCR, 0x00);
	run(avr, 10);
	check("TSM cleared: counting resumes", cr(avr, TCNT0), 30);
	check("PSR10 is cleared by hardware", cr(avr, GTCCR), 0);
	cw(avr, GTCCR, 0x01);
	check("PSR10 without TSM is cleared at once", cr(avr, GTCCR) & 1, 0);

	printf("== External clock on T0 / PB2 (Table 11-9)\n");
	avr = mk();
	cw(avr, TCCR0B, 7);			/* rising edge */
	run(avr, 100);
	check("T0 clocked: not counting on its own", cr(avr, TCNT0), 0);
	for (int i = 0; i < 5; i++) {
		avr_raise_irq(pin_irq(avr, 2), 1);
		avr_raise_irq(pin_irq(avr, 2), 0);
	}
	check("5 rising edges on T0 count 5", cr(avr, TCNT0), 5);
}

static void test_timer_outputs(void)
{
	printf("== Force Output Compare (11.9.2)\n");
	avr_t *avr = mk_pins();
	cw(avr, TCCR0A, 0x40);			/* toggle OC0A */
	cw(avr, TCCR0B, 0x01);
	cw(avr, TCCR0B, 0x81);			/* FOC0A */
	check("FOC0A toggles OC0A once", g_pin[0], 1);
	check("FOC0A reads as zero", cr(avr, TCCR0B) & 0x80, 0);
	check("FOC0A raises no interrupt flag", cr(avr, TIFR0) & 4, 0);
	cw(avr, TCCR0B, 0x81);
	check("second FOC0A toggles back", g_pin[0], 0);

	avr = mk_pins();
	cw(avr, TCCR0A, 0x20 | 0x10);		/* COM0B = 3: set on compare match */
	cw(avr, TCCR0B, 0x01);
	cw(avr, TCCR0B, 0x41);			/* FOC0B */
	check("FOC0B sets OC0B", g_pin[1], 1);

	avr = mk_pins();
	cw(avr, TCCR0A, 0x40 | 0x03);		/* fast PWM, COM0A toggle (reserved without WGM02) */
	cw(avr, TCCR0B, 0x01);
	cw(avr, TCCR0B, 0x81);
	check("FOC0A is ignored in a PWM mode", g_pin[0], 0);

	printf("== Compare Output Mode (Tables 11-2..11-4)\n");
	avr = mk_pins();
	cw(avr, OCR0A, 0x40);
	cw(avr, TCCR0A, 0x80 | 0x03);		/* fast PWM, non-inverting on OC0A */
	cw(avr, TCCR0B, 1);
	run(avr, 0x20);
	check("fast PWM: OC0A high before the match", g_pin[0], 1);
	run(avr, 0x30);
	check("fast PWM: OC0A cleared by the match", g_pin[0], 0);

	avr = mk_pins();
	cw(avr, OCR0B, 0x40);
	cw(avr, TCCR0A, 0x10 | 0x03);		/* COM0B = 1 in fast PWM: reserved */
	cw(avr, TCCR0B, 1);
	run(avr, 0x200);
	check("COM0B = toggle is reserved in PWM modes: pin untouched", g_pin[1], 0);

	avr = mk_pins();
	cw(avr, OCR0A, 0x40);
	cw(avr, TCCR0A, 0x40 | 0x03);		/* COM0A = 1, WGM02 = 0: reserved */
	cw(avr, TCCR0B, 1);
	run(avr, 0x200);
	check("COM0A = toggle without WGM02 is reserved: pin untouched", g_pin[0], 0);

	avr = mk_pins();
	cw(avr, OCR0A, 0x10);
	cw(avr, TCCR0A, 0x40 | 0x03);
	cw(avr, TCCR0B, 0x09);			/* mode 7 + toggle OC0A is allowed */
	run(avr, 0x20);				/* period 17: toggles at 17 and 34 */
	check("COM0A = toggle with WGM02 toggles at the end of each period", g_pin[0], 1);
	run(avr, 0x11);
	check("COM0A = toggle with WGM02: toggled back after the next one", g_pin[0], 0);

	printf("== Double buffered OCR0x (Table 11-8)\n");
	avr = mk_pins();
	cw(avr, OCR0A, 0x40);
	cw(avr, TCCR0A, 0x80 | 0x03);
	cw(avr, TCCR0B, 1);
	run(avr, 0x20);
	cw(avr, OCR0A, 0xf0);
	check("OCR0A reads back the new value at once", cr(avr, OCR0A), 0xf0);
	run(avr, 0x30);
	check("fast PWM: the compare still uses the old OCR0A (pin low at 0x50)", g_pin[0], 0);
	run(avr, 0x100);
	check("... the new OCR0A (0xF0) after BOTTOM: still high at 0x50 of period 2", g_pin[0], 1);
	run(avr, 0xa1);
	check("... cleared by the match at 0xF1", g_pin[0], 0);

	/* phase correct, non-inverting: cleared on the up-count match, set on the down-count one;
	 * OCR0A is latched at TOP, so a write on the up slope moves only the down-count match */
	avr = mk_pins();
	cw(avr, PORTB, 0x01);
	cw(avr, OCR0A, 0x40);
	cw(avr, TCCR0A, 0x80 | 0x01);
	cw(avr, TCCR0B, 1);
	run(avr, 0x20);
	check("PC: high before the up-count match", g_pin[0], 1);
	run(avr, 0x30);
	check("PC: cleared by the up-count match (t = 0x50)", g_pin[0], 0);
	cw(avr, OCR0A, 0x80);
	run(avr, 380 - 0x50);
	check("PC: new OCR0A not used yet on the up slope; low at t = 380", g_pin[0], 0);
	run(avr, 10);
	check("PC: down-count match uses the latched 0x80 (set at t = 383)", g_pin[0], 1);

	printf("== PWM extreme values (11.7.3, 11.7.4)\n");
	long hi;
#define DUTY(_a2, _b, _ocr, _init, _label, _want) do { \
		avr = mk_pins(); \
		cw(avr, PORTB, _init); \
		cw(avr, OCR0A, _ocr); \
		cw(avr, TCCR0A, _a2); \
		cw(avr, TCCR0B, _b); \
		run(avr, 300); /* past the first period */ \
		hi = 0; \
		for (int i = 0; i < 1020; i++) { run(avr, 1); hi += g_pin[0]; } \
		check(_label, hi, _want); \
	} while (0)
	DUTY(0x83, 1, 0, 0, "fast PWM non-inverting OCR0A = 0: one clock high per period (4)", 4);
	DUTY(0x83, 1, 255, 0, "fast PWM non-inverting OCR0A = MAX: constantly high", 1020);
	DUTY(0xc3, 1, 255, 0, "fast PWM inverting OCR0A = MAX: constantly low", 0);
	DUTY(0x81, 1, 0, 1, "phase correct non-inverting OCR0A = 0: constantly low", 0);
	DUTY(0x81, 1, 255, 1, "phase correct non-inverting OCR0A = MAX: constantly high", 1020);
	DUTY(0xc1, 1, 0, 0, "phase correct inverting OCR0A = 0: constantly high", 1020);
	DUTY(0xc1, 1, 255, 0, "phase correct inverting OCR0A = MAX: constantly low", 0);
}

static void test_adc(void)
{
	printf("== ADC conversion time (14.5, Table 14-1): 25 ADC clocks first, 13 after\n");
	for (int ps = 1; ps <= 7; ps++) {
		avr_t *avr = mk();
		long div = 1L << ps;
		char what[64];

		cw(avr, ADCSRA, 0x80 | ps);
		cw(avr, ADCSRA, 0xc0 | ps);
		run(avr, 25 * div - 1);
		snprintf(what, sizeof(what), "ADPS=%d: first conversion still running", ps);
		check(what, cr(avr, ADCSRA) & 0x10, 0);
		run(avr, 1);
		snprintf(what, sizeof(what), "ADPS=%d: first done after 25 clocks", ps);
		check(what, cr(avr, ADCSRA) & 0x10, 0x10);
		cw(avr, ADCSRA, 0x80 | 0x10 | ps);
		cw(avr, ADCSRA, 0xc0 | ps);
		run(avr, 13 * div - 1);
		snprintf(what, sizeof(what), "ADPS=%d: next still running", ps);
		check(what, cr(avr, ADCSRA) & 0x10, 0);
		run(avr, 1);
		snprintf(what, sizeof(what), "ADPS=%d: next done after 13 clocks", ps);
		check(what, cr(avr, ADCSRA) & 0x10, 0x10);
	}

	printf("== ADC result = VIN * 1024 / VREF (14.11), channels (Table 14-3), left adjust\n");
	avr_t *avr = mk();
	avr_raise_irq(avr_io_getirq(avr, AVR_IOCTL_ADC_GETIRQ, ADC_IRQ_ADC2), 2500);
	cw(avr, ADMUX, 0x02);			/* ADC2 (PB4), VCC reference */
	cw(avr, ADCSRA, 0x87);
	cw(avr, ADCSRA, 0xc7);
	run(avr, 25 * 128);
	check("2.5 V against a 5 V VCC: 512", cr(avr, ADCL) | (cr(avr, ADCH) << 8), 512);
	check("ADSC reads 0 once the conversion is complete", cr(avr, ADCSRA) & 0x40, 0);

	avr = mk();
	avr_raise_irq(avr_io_getirq(avr, AVR_IOCTL_ADC_GETIRQ, ADC_IRQ_ADC3), 550);
	cw(avr, ADMUX, 0x40 | 0x20 | 0x03);	/* ADC3 (PB3), internal 1.1 V, ADLAR */
	cw(avr, ADCSRA, 0x87);
	cw(avr, ADCSRA, 0xc7);
	run(avr, 25 * 128);
	check("0.55 V against the 1.1 V reference, left adjusted: ADCH", cr(avr, ADCH), 0x80);

	avr = mk();
	avr_raise_irq(avr_io_getirq(avr, AVR_IOCTL_ADC_GETIRQ, ADC_IRQ_ADC0), 5000);
	cw(avr, ADMUX, 0x00);
	cw(avr, ADCSRA, 0x87);
	cw(avr, ADCSRA, 0xc7);
	run(avr, 25 * 128);
	check("VIN = VREF reads 0x3FF (VREF minus one LSB)", cr(avr, ADCL) | (cr(avr, ADCH) << 8), 0x3ff);

	avr = mk();
	cw(avr, ADCSRA, 0x87);
	cw(avr, ADCSRA, 0xc7);
	cw(avr, ADCSRA, 0x87);
	check("writing ADSC = 0 does not stop a conversion", cr(avr, ADCSRA) & 0x40, 0x40);
	cw(avr, ADCSRA, 0x07);
	check("ADEN = 0 terminates it", cr(avr, ADCSRA) & 0x40, 0);

	printf("== ADC auto trigger (Table 14-5): the rising edge of the source's flag\n");
	struct { const char *n; uint8_t adts; } src[] = {
		{ "Timer0 overflow", 4 }, { "Timer0 compare match A", 3 }, { "Timer0 compare match B", 5 },
	};
	for (unsigned i = 0; i < 3; i++) {
		avr = mk();
		char what[80];
		cw(avr, OCR0A, 100);
		cw(avr, OCR0B, 150);
		cw(avr, ADCSRB, src[i].adts);
		cw(avr, ADCSRA, 0x80 | 0x20 | 0x07);	/* ADEN | ADATE | /128 */
		cw(avr, TCCR0B, 1);
		run(avr, 90);
		snprintf(what, sizeof(what), "%s: nothing started before the event", src[i].n);
		check(what, cr(avr, ADCSRA) & 0x40, 0);
		run(avr, 190);
		snprintf(what, sizeof(what), "%s: conversion started by the event", src[i].n);
		check(what, (cr(avr, ADCSRA) & 0x40) != 0 || (cr(avr, ADCSRA) & 0x10) != 0, 1);
	}

	avr = mk();
	cw(avr, ADCSRB, 4);			/* Timer0 overflow */
	cw(avr, ADCSRA, 0x80 | 0x20 | 0x01);	/* /2: a conversion is short */
	cw(avr, TCCR0B, 1);
	run(avr, 256);
	check("auto trigger from TOV0: conversion started by the overflow", cr(avr, ADCSRA) & 0x40, 0x40);
	run(avr, 60);
	check("auto trigger from TOV0: conversion completed", cr(avr, ADCSRA) & 0x10, 0x10);
	cw(avr, ADCSRA, 0x80 | 0x20 | 0x10 | 0x01);
	run(avr, 20);
	check("TOV0 left set: no new edge, no new conversion", cr(avr, ADCSRA) & 0x40, 0);
	run(avr, 256);
	check("TOV0 still set after another overflow: still no edge", cr(avr, ADCSRA) & 0x40, 0);
	cw(avr, TIFR0, 0x02);			/* clear the flag ... */
	run(avr, 256);
	check("flag cleared and set again: new conversion started and completed", cr(avr, ADCSRA) & 0x10, 0x10);

	avr = mk();
	cw(avr, ADCSRB, 6);			/* pin change interrupt request */
	cw(avr, PCMSK, 0x04);
	cw(avr, ADCSRA, 0x80 | 0x20 | 0x07);
	avr_raise_irq(pin_irq(avr, 2), 1);
	check("auto trigger from PCINT0 flag", (cr(avr, ADCSRA) & 0x40) != 0, 1);

	avr = mk();
	cw(avr, ADCSRB, 2);			/* external interrupt request 0 */
	cw(avr, MCUCR, 0x02);			/* falling edge */
	cw(avr, ADCSRA, 0x80 | 0x20 | 0x07);
	avr_raise_irq(pin_irq(avr, 1), 1);
	avr_raise_irq(pin_irq(avr, 1), 0);
	check("auto trigger from INTF0", (cr(avr, ADCSRA) & 0x40) != 0, 1);

	avr = mk();
	cw(avr, ADCSRB, 1);			/* analog comparator */
	cw(avr, ACSR, 0x02);			/* falling output edge */
	cw(avr, ADCSRA, 0x80 | 0x20 | 0x07);
	avr_raise_irq(avr_io_getirq(avr, AVR_IOCTL_ACOMP_GETIRQ, ACOMP_IRQ_AIN0), 2000);
	avr_raise_irq(avr_io_getirq(avr, AVR_IOCTL_ACOMP_GETIRQ, ACOMP_IRQ_AIN1), 1000);
	run(avr, 5);
	check("comparator output high", (cr(avr, ACSR) >> 5) & 1, 1);
	check("rising edge does not match ACIS = 10 (falling)", cr(avr, ACSR) & 0x10, 0);
	avr_raise_irq(avr_io_getirq(avr, AVR_IOCTL_ACOMP_GETIRQ, ACOMP_IRQ_AIN1), 3000);
	run(avr, 5);
	check("falling edge sets ACI", (cr(avr, ACSR) >> 4) & 1, 1);
	check("... which auto triggers the ADC", (cr(avr, ADCSRA) & 0x40) != 0, 1);
}

static void test_eeprom(void)
{
	printf("== EEPROM (5.3, 5.5.3)\n");
	avr_t *avr = mk();
	eecr_write(avr, 0x00, 5, 0x5a);
	check("atomic write: EEPE reads 1 while programming", cr(avr, EECR) & 0x02, 0x02);
	check("... and the cell is not programmed yet", ee_byte(avr, 5), 0xff);
	run(avr, 4000);
	check("3.4 ms = 4080 cycles: still busy at 4000", cr(avr, EECR) & 0x02, 0x02);
	run(avr, 100);
	check("done: EEPE cleared by hardware", cr(avr, EECR) & 0x02, 0);
	check("done: data programmed", ee_byte(avr, 5), 0x5a);

	avr = mk();
	cw(avr, EEAR, 1);
	cw(avr, EEDR, 0x11);
	cw(avr, EECR, 0x04);
	run(avr, 6);
	check("EEMPE cleared after four cycles", cr(avr, EECR) & 0x04, 0);
	cw(avr, EECR, 0x02);
	check("EEPE without EEMPE: no write", cr(avr, EECR) & 0x02, 0);

	avr = mk();
	eecr_write(avr, 0x00, 2, 0x00);
	run(avr, 4200);
	eecr_write(avr, 0x10, 2, 0x33);		/* EEPM = 01: erase only */
	check("erase only takes 1.8 ms = 2160 cycles (busy at 2100)", (run(avr, 2100), cr(avr, EECR) & 0x02), 0x02);
	run(avr, 100);
	check("erase only: cell is 0xFF, EEDR ignored", ee_byte(avr, 2), 0xff);
	eecr_write(avr, 0x20, 2, 0x33);		/* EEPM = 10: write only */
	run(avr, 2300);
	check("write only into an erased cell", ee_byte(avr, 2), 0x33);
	eecr_write(avr, 0x20, 2, 0x0f);
	run(avr, 2300);
	check("write only without erase can only clear bits", ee_byte(avr, 2), 0x03);
	eecr_write(avr, 0x30, 2, 0x77);		/* EEPM = 11: reserved */
	check("EEPM = 11 does nothing", cr(avr, EECR) & 0x02, 0);

	avr = mk();
	eecr_write(avr, 0x00, 4, 0xa5);
	cw(avr, EECR, 0x10);
	check("EEPM bits can not change while EEPE is set", cr(avr, EECR) & 0x30, 0);
	cw(avr, EEAR, 4);
	uint8_t before = cr(avr, EEDR);
	cw(avr, EECR, 0x01);
	check("EERE while programming is ignored", cr(avr, EEDR), before);

	avr = mk();
	cw(avr, EEAR, 3);
	avr_cycle_count_t c0 = avr->cycle;
	cw(avr, EECR, 0x01);
	check("EERE reads the cell", cr(avr, EEDR), 0xff);
	check("a read halts the CPU for four cycles", (long)(avr->cycle - c0), 4);
	avr = mk();
	c0 = avr->cycle;
	eecr_write(avr, 0x00, 0, 0);
	check("a write halts the CPU for two cycles", (long)(avr->cycle - c0), 2);

	printf("== EE_RDY is requested as long as EERIE is set and the EEPROM is idle\n");
	avr = mk();
	avr->sreg[S_I] = 1;
	cw(avr, EECR, 0x08);			/* EERIE with nothing programming */
	check("EERIE set while idle: interrupt pending at once", avr_has_pending_interrupts(avr), 1);
	avr->pc = 0x100;
	avr_service_interrupts(avr);
	check("... vector EE_RDY taken", avr->pc / 2, V_EE_RDY);
	avr_interrupt_reti(avr);
	check("... and requested again after RETI (constant)", avr_has_pending_interrupts(avr), 1);
	cw(avr, EECR, 0x00);

	/* EE_RDY dropped by a sleep mode without a clock for it is requested again after the wake-up */
	avr = mk();
	avr->sreg[S_I] = 1;
	cw(avr, MCUCR, 0x20 | 0x10);		/* SE, Power-down */
	eecr_write(avr, 0x00, 0, 0x12);
	cw(avr, EECR, 0x08 | 0x00);		/* EERIE (EEPE stays busy) */
	exec(avr, 0x9588);
	run(avr, 5000);
	check("EE_RDY dropped in Power-down: still asleep", avr->state, cpu_Sleeping);
	cw(avr, GIMSK, 0x20);
	cw(avr, PCMSK, 0x04);
	avr_raise_irq(pin_irq(avr, 2), 1);
	avr->state = cpu_Running;
	cw(avr, MCUCR, 0x00);
	run(avr, 200);
	check("... and EE_RDY is pending after the wake-up", avr_has_pending_interrupts(avr) != 0, 1);
}

static void test_ports_and_ints(void)
{
	printf("== Ports (10.2, 10.4)\n");
	avr_t *avr = mk();
	cw(avr, PORTB, 0x08);			/* pull-up on PB3 */
	check("pull-up reads 1 on a floating input", (cr(avr, PINB) >> 3) & 1, 1);
	avr = mk();
	cw(avr, MCUCR, 0x40);			/* PUD */
	cw(avr, PORTB, 0x08);
	check("PUD: no pull-up, input reads 0", (cr(avr, PINB) >> 3) & 1, 0);
	avr = mk();
	cw(avr, DDRB, 0x00);
	cw(avr, PORTB, 0x00);
	cw(avr, PINB, 0x15);
	check("writing 1s to PINB toggles PORTB", cr(avr, PORTB), 0x15);
	avr = mk();
	avr_raise_irq(pin_irq(avr, 0), 1);
	avr_raise_irq(pin_irq(avr, 1), 1);
	check("both pins high", cr(avr, PINB) & 3, 3);
	cw(avr, DIDR0, 0x01);
	check("DIDR0.AIN0D: PB0 reads as zero (13.2.3)", cr(avr, PINB) & 3, 2);
	cw(avr, DIDR0, 0x02);
	check("DIDR0.AIN1D: PB1 reads as zero", cr(avr, PINB) & 3, 1);

	printf("== External interrupts (9.2, 9.3)\n");
	avr = mk();
	cw(avr, DDRB, 0x02);			/* PB1 / INT0 is an output */
	cw(avr, MCUCR, 0x01);			/* any change */
	cw(avr, GIMSK, 0x40);
	cw(avr, PORTB, 0x02);
	check("INT0 triggers even when PB1 is an output", cr(avr, GIFR) & 0x40, 0x40);
	avr = mk();
	cw(avr, DDRB, 0x08);
	cw(avr, PCMSK, 0x08);
	cw(avr, GIMSK, 0x20);
	cw(avr, PORTB, 0x08);
	check("PCINT triggers even when the pin is an output", cr(avr, GIFR) & 0x20, 0x20);
	cw(avr, GIFR, 0x20);
	check("PCIF cleared by writing a one", cr(avr, GIFR) & 0x20, 0);

	avr = mk();
	cw(avr, MCUCR, 0x02);			/* falling edge */
	avr_raise_irq(pin_irq(avr, 1), 1);
	check("falling edge: rising edge ignored", cr(avr, GIFR) & 0x40, 0);
	avr_raise_irq(pin_irq(avr, 1), 0);
	check("falling edge: INTF0 set", cr(avr, GIFR) & 0x40, 0x40);
	avr = mk();
	cw(avr, MCUCR, 0x03);			/* rising edge */
	avr_raise_irq(pin_irq(avr, 1), 1);
	check("rising edge: INTF0 set", cr(avr, GIFR) & 0x40, 0x40);
}

/* Cycles one hand-assembled instruction takes: Table 21 (section 21 instruction set summary) */
static long insn_cycles(avr_t *avr, uint16_t op, int has_word2)
{
	avr->flash[0x100] = op & 0xff;
	avr->flash[0x101] = op >> 8;
	avr->flash[0x102] = has_word2 ? 0x60 : 0;	/* LDS / STS address 0x0060 */
	avr->flash[0x103] = 0;
	avr->flash[0x104] = avr->flash[0x105] = 0;
	avr->pc = 0x100;
	avr_cycle_count_t c0 = avr->cycle;
	avr->pc = avr_run_one(avr);
	long n = (long)(avr->cycle - c0);
	avr->flash[0x100] = avr->flash[0x101] = avr->flash[0x102] = 0;
	return n;
}

static void test_instruction_timing(void)
{
	printf("== Instruction timing (Table 21)\n");
	avr_t *avr = mk();
	avr->data[avr->arch.sp_addr] = 0x90;
	avr->data[26] = 0x70; avr->data[27] = 0x00;	/* X, Y, Z -> SRAM */
	avr->data[28] = 0x70; avr->data[29] = 0x00;
	avr->data[30] = 0x70; avr->data[31] = 0x00;
	struct { const char *n; uint16_t op; int w2; long want; } t[] = {
		{ "NOP", 0x0000, 0, 1 }, { "ADD", 0x0c00, 0, 1 }, { "ADIW", 0x9601, 0, 2 }, { "SBIW", 0x9701, 0, 2 },
		{ "MOVW", 0x0112, 0, 1 }, { "RJMP", 0xc000, 0, 2 }, { "IJMP", 0x9409, 0, 2 },
		{ "RCALL", 0xd000, 0, 3 }, { "ICALL", 0x9509, 0, 3 }, { "RET", 0x9508, 0, 4 }, { "RETI", 0x9518, 0, 4 },
		{ "LD Rd,X", 0x900c, 0, 2 }, { "LD Rd,X+", 0x900d, 0, 2 }, { "LD Rd,-X", 0x900e, 0, 2 },
		{ "LD Rd,Y+", 0x9009, 0, 2 }, { "LD Rd,Z", 0x8000, 0, 2 }, { "LDD Rd,Y+1", 0x8009, 0, 2 },
		{ "ST X,Rr", 0x920c, 0, 2 }, { "ST Z+,Rr", 0x9201, 0, 2 }, { "STD Y+1,Rr", 0x8209, 0, 2 },
		{ "LDS", 0x9000, 1, 2 }, { "STS", 0x9200, 1, 2 }, { "PUSH", 0x920f, 0, 2 }, { "POP", 0x900f, 0, 2 },
		{ "LPM", 0x95c8, 0, 3 }, { "LPM Rd,Z", 0x9004, 0, 3 }, { "LPM Rd,Z+", 0x9005, 0, 3 },
		{ "IN", 0xb206, 0, 1 }, { "OUT", 0xba06, 0, 1 }, { "SBI", 0x9ac0, 0, 2 }, { "CBI", 0x98c0, 0, 2 },
		{ "SLEEP-less BSET", 0x9408, 0, 1 }, { "WDR", 0x95a8, 0, 1 },
	};
	for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
		avr->data[avr->arch.sp_addr] = 0x90;
		check(t[i].n, insn_cycles(avr, t[i].op, t[i].w2), t[i].want);
	}
	avr->sreg[S_Z] = 0;
	check("BREQ not taken", insn_cycles(avr, 0xf001, 0), 1);
	avr->sreg[S_Z] = 1;
	check("BREQ taken", insn_cycles(avr, 0xf001, 0), 2);
	check("CPSE equal, skipping a one-word instruction", insn_cycles(avr, 0x1000, 0), 2);
	avr->flash[0x102] = 0x00; avr->flash[0x103] = 0x90;	/* next instruction is the two-word LDS */
	avr->pc = 0x100;
	avr->flash[0x100] = 0x00; avr->flash[0x101] = 0x10;
	avr_cycle_count_t c0 = avr->cycle;
	avr->pc = avr_run_one(avr);
	check("CPSE equal, skipping a two-word instruction", (long)(avr->cycle - c0), 3);
	avr->flash[0x100] = avr->flash[0x101] = avr->flash[0x102] = avr->flash[0x103] = 0;
}

static void test_sleep(void)
{
	printf("== Sleep modes (7.1, Table 7-1)\n");
	avr_t *avr = mk();
	avr->sreg[S_I] = 1;
	exec(avr, 0x9588);			/* SLEEP */
	check("SLEEP with SE = 0 does nothing", avr->state, cpu_Running);
	cw(avr, MCUCR, 0x20);			/* SE, Idle */
	exec(avr, 0x9588);
	check("SLEEP with SE = 1 sleeps", avr->state, cpu_Sleeping);
	cw(avr, TIMSK0, 0x02);
	cw(avr, TCCR0B, 1);
	run(avr, 300);
	check("Idle: Timer0 overflow wakes the CPU", avr->state, cpu_Running);

	avr = mk();
	avr->sreg[S_I] = 1;
	cw(avr, MCUCR, 0x20 | 0x10);		/* SE, SM = 10: Power-down */
	cw(avr, TIMSK0, 0x02);
	cw(avr, TCCR0B, 1);
	exec(avr, 0x9588);
	check("Power-down: asleep", avr->state, cpu_Sleeping);
	avr_raise_interrupt(avr, &((avr_int_vector_t){ .vector = V_TIM0_OVF }));
	run(avr, 300);
	check("Power-down: Timer0 (clkI/O stopped) does not wake the CPU", avr->state, cpu_Sleeping);
	check("Power-down: TOV0 not set either", cr(avr, TIFR0) & 2, 0);
	cw(avr, GIMSK, 0x20);
	cw(avr, PCMSK, 0x04);
	avr_raise_irq(pin_irq(avr, 2), 1);
	check("Power-down: pin change wakes the CPU", avr->state, cpu_Running);

	avr = mk();
	avr->sreg[S_I] = 1;
	cw(avr, MCUCR, 0x20 | 0x10 | 0x02);	/* Power-down, INT0 on falling edge */
	cw(avr, GIMSK, 0x40);
	exec(avr, 0x9588);
	avr_raise_irq(pin_irq(avr, 1), 1);
	avr_raise_irq(pin_irq(avr, 1), 0);
	check("Power-down: an INT0 edge does not wake (needs clkI/O)", avr->state, cpu_Sleeping);

	avr = mk();
	avr->sreg[S_I] = 1;
	cw(avr, MCUCR, 0x20 | 0x10);		/* Power-down, INT0 low level */
	cw(avr, GIMSK, 0x40);
	exec(avr, 0x9588);
	avr_raise_irq(pin_irq(avr, 1), 1);
	avr_raise_irq(pin_irq(avr, 1), 0);
	check("Power-down: an INT0 low level wakes the CPU", avr->state, cpu_Running);

	avr = mk();
	avr->sreg[S_I] = 1;
	cw(avr, MCUCR, 0x20 | 0x10);
	cw(avr, WDTCR, 0x40);			/* watchdog interrupt */
	exec(avr, 0x9588);
	run(avr, 19400);
	check("Power-down: the watchdog interrupt wakes the CPU", avr->state, cpu_Running);

	avr = mk();
	avr->sreg[S_I] = 1;
	cw(avr, MCUCR, 0x20 | 0x08);		/* ADC Noise Reduction */
	cw(avr, ADCSRA, 0x80 | 0x08 | 0x01);
	cw(avr, ADCSRA, 0xc0 | 0x08 | 0x01);
	exec(avr, 0x9588);
	run(avr, 100);
	check("ADC Noise Reduction: the ADC interrupt wakes the CPU", avr->state, cpu_Running);
}

static void test_selfprog(void)
{
	printf("== Self-programming (section 16)\n");
	avr_t *avr = mk();
	cw(avr, SPMCSR, 0x01);
	check("SPM disabled by default (SELFPRGEN fuse unprogrammed): SPMCSR ignores writes", cr(avr, SPMCSR), 0);

	avr = avr_make_mcu_by_name("attiny13a");
	avr->log = LOG_ERROR;
	avr->frequency = F_CPU;
	avr->fuse[1] &= ~(1 << 4);		/* SELFPRGEN programmed */
	avr_init(avr);
	avr->sleep = nosleep;
	cw(avr, SPMCSR, 0x01);
	check("SELFPRGEN fuse programmed: the bit can be set", cr(avr, SPMCSR) & 1, 1);
	run(avr, 8);
	check("... and is cleared again if no SPM follows within four cycles", cr(avr, SPMCSR) & 1, 0);

	/* page 3 = bytes 0x60..0x7f: erase, fill the 16 word buffer, write */
	avr->data[30] = 0x60; avr->data[31] = 0x00;	/* Z */
	cw(avr, SPMCSR, 0x03);			/* PGERS | SELFPRGEN */
	exec(avr, 0x95e8);			/* SPM */
	check("page erase", avr->flash[0x60], 0xff);
	for (int i = 0; i < 16; i++) {
		avr->data[30] = 0x60 + 2 * i;
		avr->data[0] = 0xa0 + i; avr->data[1] = 0x50 + i;	/* R1:R0 */
		cw(avr, SPMCSR, 0x01);
		exec(avr, 0x95e8);
	}
	avr->data[30] = 0x60;
	cw(avr, SPMCSR, 0x05);			/* PGWRT | SELFPRGEN */
	exec(avr, 0x95e8);
	check("page write: first byte", avr->flash[0x60], 0xa0);
	check("page write: first word's high byte", avr->flash[0x61], 0x50);
	check("page write: last word", avr->flash[0x7e], 0xaf);

	avr->data[0] = 0x12; avr->data[1] = 0x34;
	avr->data[30] = 0x00;
	cw(avr, SPMCSR, 0x01);
	exec(avr, 0x95e8);			/* fill one word */
	cw(avr, SPMCSR, 0x10 | 0x01);		/* CTPB: clear the temporary buffer */
	exec(avr, 0x95e8);
	avr->data[30] = 0x60;
	cw(avr, SPMCSR, 0x03);
	exec(avr, 0x95e8);
	cw(avr, SPMCSR, 0x05);
	exec(avr, 0x95e8);
	check("CTPB emptied the buffer: written page is erased", avr->flash[0x60], 0xff);
}

int main(void)
{
	test_cores();
	test_registers();
	test_watchdog();
	test_timer_basics();
	test_timer_outputs();
	test_adc();
	test_eeprom();
	test_ports_and_ints();
	test_instruction_timing();
	test_sleep();
	test_selfprog();

	printf("\n%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
	return failures ? 1 : 0;
}
