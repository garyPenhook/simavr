/*
 * test_sim_host.c — host API (sim_host.h) smoke test.
 *
 * Pure host code, no avr-gcc: programs are hand-assembled into flash, the
 * same way test_avrxt_engine.c does it. Exercises lifecycle, manual and
 * threaded run control, breakpoints, memory/CPU access, pins, USART and the
 * event queue on atmega4809.
 *
 *   OBJ=$(ls -d simavr/obj-*)
 *   cc -I simavr/sim -o /tmp/th tests/test_sim_host.c -L "$OBJ" -lsimavr \
 *       -lelf -lpthread
 *   LD_LIBRARY_PATH="$OBJ" /tmp/th
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <poll.h>
#include <unistd.h>
#include <pthread.h>

#include "sim_avr.h"
#include "sim_io.h"
#include "avr_ioport.h"
#include "sim_host.h"

static int g_fail, g_pass;

#define CHECK(name, cond) do { \
	if (cond) g_pass++; \
	else { g_fail++; printf("FAIL: %s (%s:%d)\n", name, __FILE__, __LINE__); } \
} while (0)

/* atmega4809 addresses, as wired in cores/sim_megax08.h */
enum { PORTA = 0x0400, PORTA_DIR = PORTA + 0x00, PORTA_IN = PORTA + 0x08 };
enum { USART0 = 0x0800, U_RXDATAL = 0x00, U_TXDATAL = 0x02, U_CTRLB = 0x06 };
enum { U_RXEN = 0x80, U_TXEN = 0x40 };	/* as in avr_usart_modern.c */

/* Opcode encodings, as used in test_avrxt_engine.c */
#define OP_LDI_R16(k)	(0xe000 | (((k) & 0xf0) << 4) | ((k) & 0x0f))
#define OP_STS_R16	0x9300		/* followed by the 16-bit address */
#define OP_SBI_VPA_OUT(b)	(0x9a08 | (b))	/* SBI VPORTA.OUT, b */
#define OP_CBI_VPA_OUT(b)	(0x9808 | (b))
#define OP_RJMP(k)	(0xc000 | ((k) & 0x0fff))
#define OP_SLEEP	0x9588
#define OP_NOP		0x0000

/*
 * 0x00 LDI  r16, 0x01
 * 0x02 STS  PORTA.DIR, r16	; PA0 output
 * 0x06 SBI  VPORTA.OUT, 0
 * 0x08 CBI  VPORTA.OUT, 0
 * 0x0a RJMP 0x06
 */
static const uint16_t blink[] = {
	OP_LDI_R16(0x01),
	OP_STS_R16, PORTA_DIR,
	OP_SBI_VPA_OUT(0),
	OP_CBI_VPA_OUT(0),
	OP_RJMP(-3),
};

static int
load_blink(
		avr_host_t * h)
{
	uint8_t img[sizeof(blink)];
	for (size_t i = 0; i < sizeof(blink) / 2; i++) {
		img[i * 2] = blink[i] & 0xff;
		img[i * 2 + 1] = blink[i] >> 8;
	}
	return avr_host_load_flash(h, img, sizeof(img), 0);
}

static int
count_events(
		avr_host_t * h,
		uint32_t type,
		int sub,
		avr_host_event_t * last)
{
	avr_host_event_t ev[256];
	int n, total = 0;
	while ((n = avr_host_poll_events(h, ev, 256)) > 0)
		for (int i = 0; i < n; i++)
			if (ev[i].type == type && (sub < 0 || ev[i].sub == sub)) {
				total++;
				if (last)
					*last = ev[i];
			}
	return total;
}

static void
sleep_ms(
		int ms)
{
	struct timespec d = { ms / 1000, (long)(ms % 1000) * 1000000L };
	nanosleep(&d, NULL);
}

static void
test_discovery(void)
{
	printf("== discovery / creation ==\n");
	CHECK("api version is draft", avr_host_api_version() == 0);
	int n = avr_host_core_count(), found = 0;
	CHECK("cores registered", n > 0);
	for (int i = 0; i < n; i++)
		if (!strcmp(avr_host_core_name(i), "atmega4809"))
			found = 1;
	CHECK("atmega4809 listed", found);
	CHECK("out-of-range core name is NULL", avr_host_core_name(n) == NULL);

	CHECK("unknown core rejected", avr_host_new("atmega9999", 0) == NULL);
	CHECK("creation error reported",
		  strstr(avr_host_last_error(NULL), "atmega9999") != NULL &&
		  avr_host_last_error_code(NULL) == AVR_HOST_ENOENT);
	CHECK("missing ELF rejected",
		  avr_host_new_from_elf("/nonexistent.elf", "atmega4809", 0) == NULL);
}

static void
test_manual(void)
{
	printf("== manual mode: step / breakpoints / pins ==\n");
	avr_host_t * h = avr_host_new("atmega4809", 16000000);
	CHECK("instance created", h != NULL);
	if (!h)
		return;
	CHECK("mmcu", !strcmp(avr_host_mmcu(h), "atmega4809"));
	/* Modern core: CLKCTRL owns the clock, so freq_hz is overridden. */
	CHECK("frequency from CLKCTRL",
		  avr_host_frequency(h) == avr_host_raw_avr(h)->frequency &&
		  avr_host_frequency(h) > 0);
	CHECK("starts paused", avr_host_get_state(h) == AVR_HOST_PAUSED);
	CHECK("resume needs a thread", avr_host_resume(h) == AVR_HOST_ESTATE);
	CHECK("load flash", load_blink(h) == 0);

	avr_host_cpu_t cpu;
	CHECK("step 1", avr_host_step(h, 1) == 0);
	CHECK("get cpu", avr_host_get_cpu(h, &cpu) == 0);
	CHECK("pc after LDI", cpu.pc == 2);
	CHECK("r16 after LDI", cpu.r[16] == 0x01);
	avr_host_step(h, 1);
	avr_host_get_cpu(h, &cpu);
	CHECK("pc after 2-word STS", cpu.pc == 6);

	int pa0 = avr_host_pin_lookup(h, "PA0");
	CHECK("PA0 lookup", pa0 >= 0);
	CHECK("pin name round-trips", !strcmp(avr_host_pin_name(h, pa0), "PA0"));
	CHECK("lowercase lookup", avr_host_pin_lookup(h, "pa0") == pa0);
	CHECK("bad pin rejected", avr_host_pin_lookup(h, "PZ9") < 0);
	avr_host_pin_t pin;
	avr_host_pin_get(h, pa0, &pin);
	CHECK("PA0 is output", pin.dir == 1 && pin.out == 0);

	int sub = avr_host_subscribe_pin(h, pa0, 0);
	CHECK("subscribe PA0", sub > 0);
	avr_host_event_t ev;
	avr_host_step(h, 1);				/* SBI */
	avr_host_pin_get(h, pa0, &pin);
	CHECK("PA0 high after SBI", pin.out == 1 && pin.in == 1);
	CHECK("PIN event for rising edge",
		  count_events(h, AVR_HOST_EV_PIN, sub, &ev) == 1 && ev.b == 1 &&
		  ev.a == (uint32_t)pa0);
	avr_host_step(h, 1);				/* CBI */
	CHECK("PIN event for falling edge",
		  count_events(h, AVR_HOST_EV_PIN, sub, &ev) == 1 && ev.b == 0);

	/* Breakpoint on the CBI: run until it is the next instruction. */
	int bp = avr_host_break_add(h, 0x08);
	CHECK("odd breakpoint pc rejected", avr_host_break_add(h, 0x07) < 0);
	CHECK("break add", bp > 0);
	CHECK("run_cycles", avr_host_run_cycles(h, 1000) == 0);
	avr_host_get_cpu(h, &cpu);
	CHECK("stopped at breakpoint", cpu.pc == 0x08);
	CHECK("breakpoint STATE event",
		  count_events(h, AVR_HOST_EV_STATE, 0, &ev) >= 1 &&
		  ev.b == AVR_HOST_STOP_BREAKPOINT && ev.c == (uint32_t)bp);
	CHECK("break remove", avr_host_break_remove(h, bp) == 0);
	CHECK("break remove twice fails", avr_host_break_remove(h, bp) < 0);

	uint64_t c0 = avr_host_cycle(h);
	avr_host_run_cycles(h, 500);
	CHECK("run_cycles advances >= 500", avr_host_cycle(h) - c0 >= 500);
	CHECK("paused after run_cycles", avr_host_get_state(h) == AVR_HOST_PAUSED);
	CHECK("time_ns follows cycle",
		  avr_host_time_ns(h) ==
		  (uint64_t)((double)avr_host_cycle(h) * 1e9 / avr_host_frequency(h)));

	/* Event queue overflow: 2 edges per 3-instruction loop, never polled. */
	count_events(h, AVR_HOST_EV_PIN, -1, NULL);
	avr_host_run_cycles(h, 100000);
	avr_host_event_t evs[8192];
	int n = avr_host_poll_events(h, evs, 8192);
	CHECK("queue capped, overflow reported",
		  n > 1 && evs[n - 1].type == AVR_HOST_EV_OVERFLOW && evs[n - 1].a > 0);
	CHECK("queue empty after drain", avr_host_poll_events(h, evs, 1) == 0);

	/* Coalesced subscription: at most one event per 10000 cycles. */
	CHECK("unsubscribe", avr_host_unsubscribe(h, sub) == 0);
	CHECK("unsubscribe twice fails", avr_host_unsubscribe(h, sub) < 0);
	sub = avr_host_subscribe_pin(h, pa0, 10000);
	avr_host_run_cycles(h, 100000);
	n = count_events(h, AVR_HOST_EV_PIN, sub, NULL);
	CHECK("coalesced pin events bounded", n >= 5 && n <= 11);
	avr_host_unsubscribe(h, sub);

	/* External drive on an input pin. */
	int pa1 = avr_host_pin_lookup(h, "PA1");
	avr_host_pin_get(h, pa1, &pin);
	CHECK("PA1 input, floating", pin.dir == 0 && pin.floating == 1);
	CHECK("drive PA1 high", avr_host_pin_drive(h, pa1, 1) == 0);
	avr_host_pin_get(h, pa1, &pin);
	CHECK("PA1 reads driven high", pin.in == 1 && pin.driven == 1 &&
		  pin.floating == 0);
	uint8_t v = 0;
	avr_host_mem_read(h, AVR_HOST_MEM_DATA, PORTA_IN, &v, 1, 1);
	CHECK("PORTA.IN bit1 set (firmware view)", (v & 0x02) != 0);
	avr_host_reset(h);
	avr_host_mem_read(h, AVR_HOST_MEM_DATA, PORTA_IN, &v, 1, 0);
	CHECK("drive survives reset (raw PORTA.IN)", (v & 0x02) != 0);
	avr_host_get_cpu(h, &cpu);
	CHECK("reset returns pc to 0", cpu.pc == 0);
	avr_host_pin_drive(h, pa1, 0);
	avr_host_mem_read(h, AVR_HOST_MEM_DATA, PORTA_IN, &v, 1, 1);
	CHECK("drive PA1 low", (v & 0x02) == 0);
	CHECK("release PA1", avr_host_pin_release(h, pa1) == 0);
	avr_host_pin_get(h, pa1, &pin);
	CHECK("PA1 no longer driven", pin.driven == 0);

	avr_host_free(h);
}

static void
test_memory(void)
{
	printf("== memory / cpu ==\n");
	avr_host_t * h = avr_host_new("atmega4809", 0);
	avr_t * avr = avr_host_raw_avr(h);
	uint32_t size;
	uint8_t b[4];

	CHECK("flash size", avr_host_mem_size(h, AVR_HOST_MEM_FLASH, &size) == 0 &&
		  size == avr->flashend + 1);
	CHECK("eeprom size", avr_host_mem_size(h, AVR_HOST_MEM_EEPROM, &size) == 0 &&
		  size == (uint32_t)(avr->arch.persist_end - avr->arch.persist_start + 1));
	CHECK("signature size", avr_host_mem_size(h, AVR_HOST_MEM_SIGNATURE, &size) == 0 &&
		  size == 3);
	CHECK("data size covers mapped flash",
		  avr_host_mem_size(h, AVR_HOST_MEM_DATA, &size) == 0 && size == 0x10000);

	uint8_t pat[2] = { 0x5a, 0xa5 };
	CHECK("flash write", avr_host_mem_write(h, AVR_HOST_MEM_FLASH, 0x100, pat, 2, 0) == 0);
	CHECK("mapped flash read via DATA",
		  avr_host_mem_read(h, AVR_HOST_MEM_DATA,
				avr->arch.flashmap_start + 0x100, b, 2, 0) == 0 &&
		  b[0] == 0x5a && b[1] == 0xa5);
	CHECK("mapped flash not writable raw",
		  avr_host_mem_write(h, AVR_HOST_MEM_DATA,
				avr->arch.flashmap_start, pat, 1, 0) == AVR_HOST_ERANGE);
	CHECK("flash out of range",
		  avr_host_mem_read(h, AVR_HOST_MEM_FLASH, avr->flashend, b, 2, 0) ==
		  AVR_HOST_ERANGE);

	CHECK("eeprom write", avr_host_mem_write(h, AVR_HOST_MEM_EEPROM, 0, pat, 2, 0) == 0);
	avr_host_reset(h);
	CHECK("eeprom survives reset",
		  avr_host_mem_read(h, AVR_HOST_MEM_EEPROM, 0, b, 2, 0) == 0 &&
		  b[0] == 0x5a && b[1] == 0xa5);

	avr_host_cpu_t cpu;
	avr_host_get_cpu(h, &cpu);
	cpu.sreg = 0x81;
	cpu.r[5] = 0x77;
	cpu.pc = 0x10;
	CHECK("set cpu", avr_host_set_cpu(h, &cpu) == 0);
	avr_host_mem_read(h, AVR_HOST_MEM_DATA, avr->arch.sreg_addr, b, 1, 0);
	CHECK("SREG visible in data space", b[0] == 0x81);
	avr_host_get_cpu(h, &cpu);
	CHECK("cpu round-trip", cpu.sreg == 0x81 && cpu.r[5] == 0x77 && cpu.pc == 0x10);
	cpu.pc = 0x11;
	CHECK("odd pc rejected", avr_host_set_cpu(h, &cpu) == AVR_HOST_ERANGE);

	avr_host_free(h);
}

static void
test_watch(void)
{
	printf("== data watchpoints ==\n");
	avr_host_t * h = avr_host_new("atmega4809", 16000000);
	CHECK("instance created", h != NULL);
	if (!h)
		return;
	CHECK("load flash", load_blink(h) == 0);

	avr_host_cpu_t cpu;
	avr_host_event_t ev;

	CHECK("watch without READ/WRITE rejected",
		  avr_host_watch_add(h, PORTA_DIR, 1, 0) == AVR_HOST_EINVAL);
	CHECK("zero-length watch rejected",
		  avr_host_watch_add(h, PORTA_DIR, 0, AVR_HOST_WATCH_WRITE) == AVR_HOST_ERANGE);
	CHECK("watch beyond data space rejected",
		  avr_host_watch_add(h, 0xffff, 1, AVR_HOST_WATCH_WRITE) == AVR_HOST_ERANGE);

	/* The STS to PORTA.DIR is the second instruction; the stop is after it. */
	int w = avr_host_watch_add(h, PORTA_DIR, 1, AVR_HOST_WATCH_WRITE);
	CHECK("watch add", w > 0);
	CHECK("run_cycles", avr_host_run_cycles(h, 1000) == 0);
	avr_host_get_cpu(h, &cpu);
	CHECK("stopped after the STS", cpu.pc == 0x06);
	CHECK("watchpoint STATE event",
		  count_events(h, AVR_HOST_EV_STATE, 0, &ev) >= 1 &&
		  ev.b == AVR_HOST_STOP_WATCHPOINT && ev.c == (uint32_t)w);

	/* Resuming does not re-hit; the loop never writes DIR again. */
	avr_host_run_cycles(h, 200);
	avr_host_get_cpu(h, &cpu);
	CHECK("no re-hit without another access",
		  count_events(h, AVR_HOST_EV_STATE, 0, &ev) >= 1 &&
		  ev.b == AVR_HOST_STOP_STEP && cpu.cycle >= 200);
	CHECK("watch remove", avr_host_watch_remove(h, w) == 0);
	CHECK("watch remove twice fails", avr_host_watch_remove(h, w) < 0);

	/* SBI/CBI on VPORTA.OUT are redirected to PORTA.OUT: the watch sees the
	 * effective address. READ|WRITE both hit; WRITE|CHANGE skips a rewrite of
	 * the same value. */
	avr_host_reset(h);
	count_events(h, AVR_HOST_EV_STATE, -1, NULL);
	w = avr_host_watch_add(h, PORTA_DIR + 4, 1, AVR_HOST_WATCH_WRITE | AVR_HOST_WATCH_CHANGE);
	avr_host_run_cycles(h, 1000);
	avr_host_get_cpu(h, &cpu);
	CHECK("redirected SBI hits PORTA.OUT", cpu.pc == 0x08);
	avr_host_watch_remove(h, w);

	/* Host peeks/pokes (GUI memory panes) are not CPU accesses: they must
	 * neither record a hit nor pause the next run. */
	w = avr_host_watch_add(h, 0x2800, 1, AVR_HOST_WATCH_READ | AVR_HOST_WATCH_WRITE);
	uint8_t b = 0x55;
	avr_host_mem_write(h, AVR_HOST_MEM_DATA, 0x2800, &b, 1, 0);
	avr_host_mem_read(h, AVR_HOST_MEM_DATA, 0x2800, &b, 1, 0);
	count_events(h, AVR_HOST_EV_STATE, -1, NULL);
	avr_host_run_cycles(h, 100);
	CHECK("host peek/poke does not trigger a watch",
		  count_events(h, AVR_HOST_EV_STATE, 0, &ev) >= 1 && ev.b == AVR_HOST_STOP_STEP);
	avr_host_watch_remove(h, w);

	avr_host_free(h);
}

static void
test_uart(void)
{
	printf("== USART0 ==\n");
	avr_host_t * h = avr_host_new("atmega4809", 0);
	uint8_t v = U_RXEN | U_TXEN;
	avr_host_mem_write(h, AVR_HOST_MEM_DATA, USART0 + U_CTRLB, &v, 1, 1);

	int sub = avr_host_subscribe_uart(h, 0);
	CHECK("subscribe USART0", sub > 0);
	CHECK("no USART7", avr_host_subscribe_uart(h, 7) == AVR_HOST_ENOENT);
	v = 'H';
	avr_host_mem_write(h, AVR_HOST_MEM_DATA, USART0 + U_TXDATAL, &v, 1, 1);
	avr_host_event_t ev;
	CHECK("TX byte event", count_events(h, AVR_HOST_EV_UART_TX, sub, &ev) == 1 &&
		  ev.b == 'H' && ev.a == 0);

	/* More than the model's RX fifo holds: the rest waits in the host. */
	enum { N = 300 };
	uint8_t msg[N];
	for (int i = 0; i < N; i++)
		msg[i] = (i < 2) ? 'a' : (uint8_t)i;	/* repeated bytes must survive */
	CHECK("send", avr_host_uart_send(h, 0, msg, N) == 0);
	int got = 0, ok = 1;
	for (int round = 0; round < 4 && got < N; round++) {
		for (;;) {
			uint8_t st;
			avr_host_mem_read(h, AVR_HOST_MEM_DATA, USART0 + 0x04, &st, 1, 0);
			if (!(st & 0x80))	/* STATUS.RXCIF */
				break;
			avr_host_mem_read(h, AVR_HOST_MEM_DATA, USART0 + U_RXDATAL, &v, 1, 1);
			if (got >= N || v != msg[got])
				ok = 0;
			got++;
		}
		avr_host_run_cycles(h, 10);		/* batch end feeds pending bytes */
	}
	CHECK("all RX bytes delivered in order", got == N && ok);
	avr_host_free(h);
}

static void
test_threaded(void)
{
	printf("== threaded run control ==\n");
	avr_host_t * h = avr_host_new("atmega4809", 16000000);
	load_blink(h);
	int pa0 = avr_host_pin_lookup(h, "PA0");
	int sub = avr_host_subscribe_pin(h, pa0, 1000);
	int fd = avr_host_event_fd(h);
	CHECK("event fd", fd >= 0);

	CHECK("start", avr_host_start(h) == 0);
	CHECK("start twice fails", avr_host_start(h) == AVR_HOST_ESTATE);
	CHECK("still paused after start", avr_host_get_state(h) == AVR_HOST_PAUSED);
	CHECK("resume", avr_host_resume(h) == 0);
	CHECK("resume while running fails", avr_host_resume(h) == AVR_HOST_ESTATE);
	CHECK("running", avr_host_get_state(h) == AVR_HOST_RUNNING);
	struct timespec w0, w1;
	clock_gettime(CLOCK_MONOTONIC, &w0);
	CHECK("wait times out while running",
		  avr_host_wait_stopped(h, 20) == AVR_HOST_ESTATE);
	clock_gettime(CLOCK_MONOTONIC, &w1);
	/* Regression: a timed wait used to re-take the batch lock and could
	 * lose it to the sim thread for seconds. */
	CHECK("timed wait returns on time",
		  (w1.tv_sec - w0.tv_sec) * 1000 + (w1.tv_nsec - w0.tv_nsec) / 1000000
		  < 500);

	avr_host_cpu_t cpu;
	CHECK("cpu locked out while running",
		  avr_host_get_cpu(h, &cpu) == AVR_HOST_ESTATE);
	uint8_t dir = 0;
	CHECK("memory readable while running",
		  avr_host_mem_read(h, AVR_HOST_MEM_DATA, PORTA_DIR, &dir, 1, 0) == 0 &&
		  dir == 0x01);
	uint64_t c0 = avr_host_cycle(h);
	sleep_ms(20);
	CHECK("cycles advance while running", avr_host_cycle(h) > c0);

	struct pollfd p = { .fd = fd, .events = POLLIN };
	CHECK("event fd readable", poll(&p, 1, 1000) == 1);
	CHECK("pin events while running",
		  count_events(h, AVR_HOST_EV_PIN, sub, NULL) > 0);

	CHECK("pause", avr_host_pause(h) == 0);
	CHECK("wait stopped", avr_host_wait_stopped(h, 1000) == 0);
	/* Contract: wait_stopped() returns only after the stop's STATE event is
	 * queued (poll straight away, no locking call in between). The race this
	 * guards against is sub-microsecond, so this checks the contract rather
	 * than reliably reproducing a violation. */
	avr_host_event_t ev = { 0 };
	count_events(h, AVR_HOST_EV_STATE, 0, &ev);
	CHECK("pause STATE event queued before wait_stopped returns",
		  ev.type == AVR_HOST_EV_STATE && ev.a == AVR_HOST_PAUSED &&
		  ev.b == AVR_HOST_STOP_USER);
	CHECK("paused", avr_host_get_state(h) == AVR_HOST_PAUSED);
	p.revents = 0;
	CHECK("event fd idle after drain", poll(&p, 1, 0) == 0);

	/* Real-time throttle: ~100 ms wall should be ~frequency / 10 cycles. */
	uint64_t want = avr_host_frequency(h) / 10;
	avr_host_set_speed(h, 1.0);
	c0 = avr_host_cycle(h);
	avr_host_resume(h);
	sleep_ms(100);
	avr_host_pause(h);
	avr_host_wait_stopped(h, 1000);
	uint64_t ran = avr_host_cycle(h) - c0;
	CHECK("speed 1.0 paces near real time", ran > want / 2 && ran < want * 3 / 2);
	if (!(ran > want / 2 && ran < want * 3 / 2))
		printf("  ran %llu cycles, wanted ~%llu\n",
			   (unsigned long long)ran, (unsigned long long)want);
	avr_host_set_speed(h, 0);

	avr_host_get_cpu(h, &cpu);
	uint32_t pc0 = cpu.pc;
	CHECK("threaded step", avr_host_step(h, 1) == 0);
	CHECK("step completes", avr_host_wait_stopped(h, 1000) == 0);
	avr_host_get_cpu(h, &cpu);
	CHECK("step moved pc", cpu.pc != pc0);

	int bp = avr_host_break_add(h, 0x08);
	avr_host_resume(h);
	CHECK("breakpoint stops thread", avr_host_wait_stopped(h, 1000) == 0);
	avr_host_get_cpu(h, &cpu);
	CHECK("at breakpoint pc", cpu.pc == 0x08);
	avr_host_break_remove(h, bp);

	/* Free while running must stop the thread cleanly. */
	avr_host_resume(h);
	avr_host_free(h);
	CHECK("free while running", 1);
}

static void
test_hex(void)
{
	printf("== HEX load ==\n");
	char path[] = "/tmp/test_sim_host_XXXXXX";
	int fd = mkstemp(path);
	if (fd < 0) {
		CHECK("mkstemp", 0);
		return;
	}
	/* RJMP .-2 at 0 (bytes ff cf), then EOF record. */
	const char * hex = ":02000000FFCF30\n:00000001FF\n";
	CHECK("write hex", write(fd, hex, strlen(hex)) == (ssize_t)strlen(hex));
	close(fd);

	avr_host_t * h = avr_host_new("atmega4809", 0);
	CHECK("load hex", avr_host_load_hex(h, path) == 0);
	uint8_t b[2] = { 0, 0 };
	avr_host_mem_read(h, AVR_HOST_MEM_FLASH, 0, b, 2, 0);
	CHECK("hex bytes in flash", b[0] == 0xff && b[1] == 0xcf);
	avr_host_step(h, 3);
	avr_host_cpu_t cpu;
	avr_host_get_cpu(h, &cpu);
	CHECK("self-loop keeps pc at 0", cpu.pc == 0);
	CHECK("missing hex rejected",
		  avr_host_load_hex(h, "/nonexistent.hex") == AVR_HOST_EIO);
	avr_host_free(h);
	unlink(path);
}

static int
write_file(
		char * path,
		const char * text)
{
	int fd = mkstemp(path);
	if (fd < 0)
		return -1;
	int ok = write(fd, text, strlen(text)) == (ssize_t)strlen(text);
	close(fd);
	return ok ? 0 : -1;
}

static int
load_words(
		avr_host_t * h,
		const uint16_t * w,
		int n)
{
	uint8_t img[64];
	for (int i = 0; i < n; i++) {
		img[i * 2] = w[i] & 0xff;
		img[i * 2 + 1] = w[i] >> 8;
	}
	return avr_host_load_flash(h, img, n * 2, 0);
}

static int g_hammer;

static void *
hammer(
		void * param)
{
	while (__atomic_load_n(&g_hammer, __ATOMIC_RELAXED))
		avr_host_get_state(param);
	return NULL;
}

/* Regressions for the code-review findings on the first host API cut. */
static void
test_regressions(void)
{
	printf("== review regressions ==\n");
	avr_host_event_t ev;
	avr_host_cpu_t cpu;

	/* Corrupt HEX: second record has a bad checksum. */
	{
		char path[] = "/tmp/test_sim_host_XXXXXX";
		write_file(path, ":02000000FFCF30\n:02000200FFCF00\n:00000001FF\n");
		avr_host_t * h = avr_host_new("atmega4809", 0);
		CHECK("corrupt HEX rejected", avr_host_load_hex(h, path) == AVR_HOST_EIO);
		avr_host_free(h);
		unlink(path);
	}
	/* Modern EEPROM record (extended linear address 0x0081 -> 0x810000). */
	{
		char path[] = "/tmp/test_sim_host_XXXXXX";
		write_file(path, ":02000000FFCF30\n:02000004008179\n"
						 ":020000005AA5FF\n:00000001FF\n");
		avr_host_t * h = avr_host_new("atmega4809", 0);
		CHECK("HEX with EEPROM loads", avr_host_load_hex(h, path) == 0);
		uint8_t b[2] = { 0, 0 };
		avr_host_mem_read(h, AVR_HOST_MEM_EEPROM, 0, b, 2, 0);
		CHECK("modern EEPROM record loaded", b[0] == 0x5a && b[1] == 0xa5);
		avr_host_free(h);
		unlink(path);
	}
	/* Breakpoint at the reset vector fires; resuming executes past it. */
	{
		avr_host_t * h = avr_host_new("atmega4809", 0);
		load_blink(h);
		int bp = avr_host_break_add(h, 0);
		count_events(h, AVR_HOST_EV_STATE, 0, NULL);
		avr_host_run_cycles(h, 1000);
		avr_host_get_cpu(h, &cpu);
		CHECK("reset-vector breakpoint hits",
			  cpu.pc == 0 && count_events(h, AVR_HOST_EV_STATE, 0, &ev) == 1 &&
			  ev.b == AVR_HOST_STOP_BREAKPOINT && ev.c == (uint32_t)bp);
		avr_host_step(h, 1);
		avr_host_get_cpu(h, &cpu);
		CHECK("step off a breakpoint executes it", cpu.pc == 2);
		avr_host_set_cpu(h, &(avr_host_cpu_t){ .pc = 0, .sp = cpu.sp });
		avr_host_run_cycles(h, 1000);
		avr_host_get_cpu(h, &cpu);
		CHECK("breakpoint at a PC set by set_cpu hits", cpu.pc == 0 &&
			  count_events(h, AVR_HOST_EV_STATE, 0, &ev) >= 1 &&
			  ev.b == AVR_HOST_STOP_BREAKPOINT);
		avr_host_free(h);
	}
	/* A sleeping CPU doesn't re-hit the breakpoint on the next instruction. */
	{
		avr_host_t * h = avr_host_new("atmega328p", 0);
		if (h) {
			static const uint16_t prog[] = { OP_SLEEP, OP_NOP, OP_RJMP(-2) };
			load_words(h, prog, 3);
			avr_host_get_cpu(h, &cpu);
			cpu.sreg = 0x80;			/* I set, so SLEEP isn't "done" */
			avr_host_set_cpu(h, &cpu);
			avr_host_break_add(h, 2);
			count_events(h, AVR_HOST_EV_STATE, 0, NULL);
			avr_host_run_cycles(h, 20000);
			count_events(h, AVR_HOST_EV_STATE, 0, &ev);
			avr_host_get_cpu(h, &cpu);
			CHECK("asleep: breakpoint after SLEEP not hit",
				  ev.b == AVR_HOST_STOP_STEP && cpu.pc == 2);
			avr_host_free(h);
		} else
			printf("  (atmega328p core not built, skipped)\n");
	}
	/* Classic pin drive is latched into PINx. */
	{
		avr_host_t * h = avr_host_new("atmega328p", 0);
		if (h) {
			int pb0 = avr_host_pin_lookup(h, "PB0");
			avr_host_pin_t pin;
			avr_host_pin_drive(h, pb0, 1);
			avr_host_pin_get(h, pb0, &pin);
			avr_ioport_state_t st;
			avr_host_lock(h);
			avr_ioctl(avr_host_raw_avr(h), AVR_IOCTL_IOPORT_GETSTATE('B'), &st);
			avr_host_unlock(h);
			CHECK("classic drive reaches PINB", pin.in == 1 && (st.pin & 1));
			avr_host_pin_drive(h, pb0, 0);
			avr_host_pin_get(h, pb0, &pin);
			CHECK("classic drive low", pin.in == 0);
			avr_host_free(h);
		}
	}
	/* Side-effect DATA access into the reserved gap is refused. */
	{
		avr_host_t * h = avr_host_new("attiny3217", 0);
		avr_t * avr = avr_host_raw_avr(h);
		uint32_t gap = avr->ramend + 1;
		uint8_t v = 0x42;
		if (avr->arch.flashmap_start > gap)
			CHECK("unmapped data refused with side effects",
				  avr_host_mem_write(h, AVR_HOST_MEM_DATA, gap, &v, 1, 1) ==
				  AVR_HOST_ERANGE &&
				  avr_host_mem_read(h, AVR_HOST_MEM_DATA, gap, &v, 1, 1) ==
				  AVR_HOST_ERANGE);
		avr_host_free(h);
	}
	/* Coalesced pin values are queued before the STATE event of the stop. */
	{
		avr_host_t * h = avr_host_new("atmega4809", 0);
		load_blink(h);
		int sub = avr_host_subscribe_pin(h, avr_host_pin_lookup(h, "PA0"),
										 1000000);
		avr_host_run_cycles(h, 2000);
		avr_host_event_t evs[64];
		int n = avr_host_poll_events(h, evs, 64), pin_at = -1, state_at = -1;
		for (int i = 0; i < n; i++) {
			if (evs[i].type == AVR_HOST_EV_PIN && evs[i].sub == sub)
				pin_at = i;
			if (evs[i].type == AVR_HOST_EV_STATE && state_at < 0)
				state_at = i;
		}
		CHECK("final pin value precedes STATE",
			  pin_at >= 0 && state_at > pin_at);
		avr_host_free(h);
	}
	/* A GUI thread polling in a tight loop doesn't starve the sim. */
	{
		avr_host_t * h = avr_host_new("atmega4809", 0);
		load_blink(h);
		avr_host_start(h);
		pthread_t t;
		__atomic_store_n(&g_hammer, 1, __ATOMIC_RELAXED);
		pthread_create(&t, NULL, hammer, h);
		uint64_t c0 = avr_host_cycle(h);
		avr_host_resume(h);
		sleep_ms(100);
		avr_host_pause(h);
		avr_host_wait_stopped(h, 1000);
		__atomic_store_n(&g_hammer, 0, __ATOMIC_RELAXED);
		pthread_join(t, NULL);
		CHECK("sim progresses under tight polling",
			  avr_host_cycle(h) - c0 > avr_host_frequency(h) / 100);
		avr_host_free(h);
	}
}

int
main(void)
{
	test_discovery();
	test_manual();
	test_memory();
	test_uart();
	test_watch();
	test_threaded();
	test_hex();
	test_regressions();
	printf("%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
