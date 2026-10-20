/*
	sim_host.h

	Stable host-facing API for embedding simavr in GUIs and language
	bindings (see doc/python_gui_todo.md, Phase 1).

	STATUS: DRAFT. Names and semantics are open for review; nothing below
	is ABI-stable until AVR_HOST_API_VERSION reaches 1. Implemented in
	sim_host.c except where a section says "not implemented yet" (those
	calls return AVR_HOST_ENOTSUP).

 	This file is part of simavr.

	simavr is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	simavr is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with simavr.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __SIM_HOST_H__
#define __SIM_HOST_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

/*
 * Design rules
 * ------------
 * - Plain C ABI, opaque handles, fixed-width types, no simavr internal
 *   structs in any signature (cffi-friendly). Only avr_host_raw_avr()
 *   leaks avr_t, and it is explicitly unstable.
 *
 * - Threading model: the host owns ONE simulation thread per instance,
 *   started by avr_host_start(). That thread calls avr_run() in batches
 *   while holding the instance lock, and drains a command queue between
 *   batches. Every function here is callable from any thread:
 *     * avr_host_pause() is lock-free and honoured at the next instruction;
 *     * every other call takes the instance lock, so it waits at most one
 *       batch (see avr_host_set_batch()) and always sees a consistent
 *       snapshot between two instructions. Control calls (resume, step,
 *       run_cycles, reset) only change the run state and return; use
 *       avr_host_wait_stopped() to wait for a step to finish.
 *   Without avr_host_start() the instance is "manual": avr_host_step() and
 *   avr_host_run_cycles() execute synchronously on the caller's thread
 *   (a step on a CPU asleep with no wake-up source never returns; bound it
 *   with avr_host_run_cycles()), and avr_host_resume() is refused.
 *   Never call avr_host_* from a native IRQ callback (it runs on the sim
 *   thread with the lock held and would deadlock).
 *
 * - Events: the sim thread never calls back into the GUI. Changes are
 *   pushed into a bounded per-instance queue which the GUI drains with
 *   avr_host_poll_events(). avr_host_event_fd() gives a pollable fd
 *   (readable while the queue is non-empty) for QSocketNotifier / select.
 *   This sidesteps foreign-thread callbacks under the Python GIL.
 *   On overflow, events are dropped and one AVR_HOST_EV_OVERFLOW carrying
 *   the drop count is delivered when space frees up.
 *
 * - Errors: functions returning int return 0 on success or a negative
 *   AVR_HOST_E* code; avr_host_last_error() / avr_host_last_error_code()
 *   describe the most recent failure on the calling thread (errno-style,
 *   so concurrent callers never see each other's errors).
 */

#define AVR_HOST_API_VERSION	0	/* 0 = draft, unstable */

typedef struct avr_host_t avr_host_t;	/* opaque */

enum {
	AVR_HOST_OK		= 0,
	AVR_HOST_EINVAL	= -1,	/* bad argument / unknown name */
	AVR_HOST_ENOENT	= -2,	/* file, core, pin, register or symbol not found */
	AVR_HOST_ESTATE	= -3,	/* not allowed in current run state */
	AVR_HOST_ERANGE	= -4,	/* address/length outside the memory space */
	AVR_HOST_ENOMEM	= -5,
	AVR_HOST_EIO	= -6,	/* firmware load / parse failure */
	AVR_HOST_ENOTSUP	= -7,	/* feature not available on this core */
};

/* ---------------------------------------------------------------------
 * Library / core discovery
 * ------------------------------------------------------------------- */

int
avr_host_api_version(void);

/* Cores compiled into this libsimavr (same set as run_avr --list-cores). */
int
avr_host_core_count(void);
const char *
avr_host_core_name(
		int index);

/* ---------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------- */

/* Create an instance for 'mmcu' (e.g. "attiny3217") clocked at 'freq_hz'
 * (0 = core default). On modern cores the CLKCTRL model owns the CPU
 * clock: every reset re-derives it from the oscillator fuse and the reset
 * prescaler, and firmware changes it, so 'freq_hz' is overridden there.
 * Returns NULL on failure; avr_host_last_error_code() / _error() say why. */
avr_host_t *
avr_host_new(
		const char * mmcu,
		uint32_t freq_hz);

/* Create from an ELF, taking mmcu/frequency from its .mmcu section.
 * Fallbacks are used when the section is absent ('mmcu' may be NULL to
 * require the section). Firmware is loaded exactly as avr_host_load_elf()
 * does, including the reset. */
avr_host_t *
avr_host_new_from_elf(
		const char * path,
		const char * fallback_mmcu,
		uint32_t fallback_freq_hz);

/* Stops the sim thread (if any), then frees everything. No other thread
 * may be inside (or later enter) an avr_host_* call on 'h'. */
void
avr_host_free(
		avr_host_t * h);

/* Message / AVR_HOST_E* code of the calling thread's most recent failure
 * (any instance, including creation). 'h' is unused and may be NULL. Only
 * meaningful right after a call that failed. */
const char *
avr_host_last_error(
		avr_host_t * h);
int
avr_host_last_error_code(
		avr_host_t * h);

const char *
avr_host_mmcu(
		avr_host_t * h);
/* Current CPU clock (may change at run time on modern cores, see above). */
uint32_t
avr_host_frequency(
		avr_host_t * h);

/* ---------------------------------------------------------------------
 * Firmware
 * Loading requires the instance to be paused/stopped; it resets the MCU.
 * EEPROM data in the image is loaded too (on modern cores into the
 * data-space EEPROM). An image built as a bootloader (flash base from its
 * .mmcu/ELF info) starts at that address, like run_avr; a later
 * avr_host_reset() goes to the core's reset vector. External pulls the
 * ELF declares in .mmcu are kept and combined with host pin drive.
 * ------------------------------------------------------------------- */

int
avr_host_load_elf(
		avr_host_t * h,
		const char * path);
/* Intel HEX (multipart HEX like run_avr: flash + EEPROM records). A
 * malformed record or bad checksum fails with AVR_HOST_EIO and leaves
 * the loaded firmware untouched. */
int
avr_host_load_hex(
		avr_host_t * h,
		const char * path);
/* Raw image into flash at byte address 'addr'. */
int
avr_host_load_flash(
		avr_host_t * h,
		const uint8_t * data,
		uint32_t size,
		uint32_t addr);

/* ---------------------------------------------------------------------
 * Run control
 * ------------------------------------------------------------------- */

typedef enum avr_host_state_t {
	AVR_HOST_PAUSED = 0,	/* not executing; state accessible */
	AVR_HOST_RUNNING,		/* free-running (includes MCU sleep) */
	AVR_HOST_SLEEPING,		/* running, CPU in SLEEP waiting for an IRQ */
	AVR_HOST_DONE,			/* firmware exited (cli; sleep / avr_exit) */
	AVR_HOST_CRASHED,		/* bad opcode, stack overflow, etc. */
} avr_host_state_t;

/* Why the instance last stopped; delivered with AVR_HOST_EV_STATE. */
typedef enum avr_host_stop_reason_t {
	AVR_HOST_STOP_NONE = 0,
	AVR_HOST_STOP_USER,			/* avr_host_pause() */
	AVR_HOST_STOP_STEP,			/* step / run_cycles completed */
	AVR_HOST_STOP_BREAKPOINT,
	AVR_HOST_STOP_WATCHPOINT,
	AVR_HOST_STOP_DONE,
	AVR_HOST_STOP_CRASH,
} avr_host_stop_reason_t;

/* Spawn the sim thread. The instance starts PAUSED. */
int
avr_host_start(
		avr_host_t * h);

int
avr_host_resume(
		avr_host_t * h);
int
avr_host_pause(
		avr_host_t * h);
/* Execute 'count' instructions, then pause (AVR_HOST_STOP_STEP).
 * Stepping over a cycle where an interrupt is taken stops at the first
 * instruction of the vector. TODO: pin down AVRxt step semantics for
 * multi-cycle instructions and CPUINT level-1 preemption before v1. */
int
avr_host_step(
		avr_host_t * h,
		uint32_t count);
/* Run at least 'cycles' CPU cycles, then pause at an instruction boundary. */
int
avr_host_run_cycles(
		avr_host_t * h,
		uint64_t cycles);
/* MCU reset (power-on style). Keeps loaded firmware, breakpoints and
 * subscriptions; modern-AVR EEPROM survives, like real hardware. */
int
avr_host_reset(
		avr_host_t * h);

avr_host_state_t
avr_host_get_state(
		avr_host_t * h);

/* Block until the instance is no longer RUNNING/SLEEPING, or timeout.
 * timeout_ms < 0 waits forever. Returns AVR_HOST_ESTATE on timeout. */
int
avr_host_wait_stopped(
		avr_host_t * h,
		int timeout_ms);

/* Throttle: 1.0 = real time, 0.5 = half speed, 0 = as fast as possible
 * (default). Simulated time is unaffected; only wall-clock pacing. */
int
avr_host_set_speed(
		avr_host_t * h,
		double ratio);

/* Max cycles executed per locked batch (default ~ 1 ms of sim time).
 * Smaller = lower accessor latency, more lock overhead. Accessors called
 * in a tight loop slow the simulation down (but cannot stall it). */
int
avr_host_set_batch(
		avr_host_t * h,
		uint32_t cycles);

uint64_t
avr_host_cycle(
		avr_host_t * h);
/* Simulated time in nanoseconds, computed as cycle / current frequency;
 * only exact if the clock never changed since reset. */
uint64_t
avr_host_time_ns(
		avr_host_t * h);

/* ---------------------------------------------------------------------
 * CPU state (only while PAUSED, else AVR_HOST_ESTATE)
 * ------------------------------------------------------------------- */

typedef struct avr_host_cpu_t {
	uint32_t	pc;			/* byte address */
	uint16_t	sp;
	uint8_t		sreg;
	uint8_t		r[32];
	uint64_t	cycle;
} avr_host_cpu_t;

int
avr_host_get_cpu(
		avr_host_t * h,
		avr_host_cpu_t * out);
/* Writes pc/sp/sreg/r[] back; 'cycle' is ignored. */
int
avr_host_set_cpu(
		avr_host_t * h,
		const avr_host_cpu_t * in);

/* Interrupt vectors that are raised, enabled and waiting to be taken (the
 * engine's pending queue), in dispatch order: highest CPUINT level first
 * (modern AVR: 2 NMI, 1 LVL1VEC, 0 normal; classic is always 0), then by the
 * engine's rank within a level (lowest vector, or LVL0PRI-relative for level
 * 0). Writes up to 'max' vector numbers into 'vectors' and their levels into
 * 'levels' (either may be NULL) and returns how many are pending in total
 * (may exceed 'max'). A raised flag whose enable bit is clear is not pending,
 * and a vector already running is not pending again until re-raised. Whether
 * the first is taken next still depends on SREG.I and the executing level;
 * with LVL0RR the order of the rest shifts as each is taken.
 * Only while paused, else AVR_HOST_ESTATE. */
int
avr_host_pending_interrupts(
		avr_host_t * h,
		uint8_t * vectors,
		uint8_t * levels,
		int max);

/* ---------------------------------------------------------------------
 * Memory
 * Addresses are in each space's native byte addressing. DATA is the data
 * space as firmware sees it with LD/ST: on modern AVR that includes the
 * VPORT aliases at 0x0000 (not the register file; use avr_host_get_cpu()),
 * the mapped EEPROM, and the flash window up to 0xFFFF (read-only).
 * Addresses in no mapped region (e.g. between SRAM and the flash window)
 * fail with AVR_HOST_ERANGE.
 * With 'side_effects' set, DATA accesses run the IO callbacks exactly like
 * an LD/ST; by default they are raw, so an inspector reading e.g.
 * USARTn.RXDATAL does not pop the FIFO (raw reads of IO registers that are
 * computed on read, such as PORTx.IN for output pins, may be stale).
 * Allowed in any state (lock-protected).
 * ------------------------------------------------------------------- */

typedef enum avr_host_space_t {
	AVR_HOST_MEM_DATA = 0,
	AVR_HOST_MEM_FLASH,
	AVR_HOST_MEM_EEPROM,
	AVR_HOST_MEM_FUSES,
	AVR_HOST_MEM_SIGNATURE,
} avr_host_space_t;

int
avr_host_mem_size(
		avr_host_t * h,
		avr_host_space_t space,
		uint32_t * out_size);

int
avr_host_mem_read(
		avr_host_t * h,
		avr_host_space_t space,
		uint32_t addr,
		uint8_t * buf,
		uint32_t len,
		int side_effects);
int
avr_host_mem_write(
		avr_host_t * h,
		avr_host_space_t space,
		uint32_t addr,
		const uint8_t * buf,
		uint32_t len,
		int side_effects);

/* ---------------------------------------------------------------------
 * Register / peripheral metadata — not implemented yet
 * Static per-core table: every named IO register of the device, so GUIs
 * never scrape internal structs. Requires a generated per-core table
 * (from the avr-libc device header at core build time).
 * ------------------------------------------------------------------- */

typedef struct avr_host_reg_info_t {
	const char *	name;		/* "TCA0.SINGLE.CTRLA", "PORTA.DIR" */
	const char *	peripheral;	/* "TCA0", "PORTA" */
	uint16_t		addr;		/* data-space address */
	uint8_t			size;		/* bytes: 1, 2 (16-bit pairs), 3/4 */
	uint8_t			reset;		/* documented reset value, low byte */
} avr_host_reg_info_t;

int
avr_host_reg_count(
		avr_host_t * h);
int
avr_host_reg_info(
		avr_host_t * h,
		int index,
		avr_host_reg_info_t * out);
/* Returns the index, or a negative error. */
int
avr_host_reg_lookup(
		avr_host_t * h,
		const char * name);

/* ---------------------------------------------------------------------
 * Symbols (from the loaded ELF; empty for HEX) — not implemented yet
 * ------------------------------------------------------------------- */

int
avr_host_symbol_lookup(
		avr_host_t * h,
		const char * name,
		uint32_t * out_addr,
		uint32_t * out_size);
/* Nearest symbol at or below 'addr' in flash; 'offset' = addr - symbol. */
const char *
avr_host_symbol_at(
		avr_host_t * h,
		uint32_t flash_addr,
		uint32_t * out_offset);

/* ---------------------------------------------------------------------
 * Breakpoints / watchpoints
 * Hitting one pauses the instance and emits AVR_HOST_EV_STATE with the
 * matching stop reason and id. Ids are > 0. A breakpoint fires when its
 * instruction is about to execute (PC == its address, CPU awake), so it
 * also fires on the very first instruction of a run, e.g. at the reset
 * vector. Resuming or stepping from the PC where the instance stopped
 * executes that instruction instead of hitting it again.
 * ------------------------------------------------------------------- */

int
avr_host_break_add(
		avr_host_t * h,
		uint32_t pc);
int
avr_host_break_remove(
		avr_host_t * h,
		int id);

enum {
	AVR_HOST_WATCH_READ	= 1 << 0,
	AVR_HOST_WATCH_WRITE	= 1 << 1,
	/* Pause only if the written value differs from the old one. */
	AVR_HOST_WATCH_CHANGE	= 1 << 2,
	/* Pause when any byte of the range differs after an instruction (or the
	 * timers/peripherals run), whoever wrote it: CPU, peripheral model or
	 * interrupt hardware. Needs neither READ nor WRITE; the range is re-
	 * baselined on every resume, so host pokes never fire it. A write of the
	 * same value is not a change. */
	AVR_HOST_WATCH_PERIPH	= 1 << 3,
};

/* Pausing data-space watch over [addr, addr+len) (addr is a data-space
 * address: registers, I/O and SRAM). Fires on CPU accesses only (LD/ST/LDS/
 * STS/IN/OUT/SBI/PUSH/CALL/interrupt entry/...); writes made by peripheral models are not seen
 * unless AVR_HOST_WATCH_PERIPH is set. The
 * instruction completes first, so the instance stops with PC at the next
 * instruction (AVR_HOST_STOP_WATCHPOINT, id = the watch id). A read-modify-
 * write instruction (SBI/CBI) counts as both a read and a write. The
 * non-pausing AVR_HOST_EV_WATCH event is not produced. Errors: ERANGE (outside
 * data space), EINVAL (none of READ, WRITE, PERIPH), ENOMEM (32 watches). */
int
avr_host_watch_add(
		avr_host_t * h,
		uint16_t addr,
		uint16_t len,
		uint32_t flags);
int
avr_host_watch_remove(
		avr_host_t * h,
		int id);

/* ---------------------------------------------------------------------
 * Pins
 * Pins are addressed by name ("PA3", "PB0") and resolved to a small
 * integer id once. State reflects the PORT model: what firmware drives
 * and what the outside world sees.
 * KNOWN GAP: PORTMUX routing is not modelled, so a peripheral output
 * (USART TXD, TCA WOn, ...) does not appear on its physical pin yet; use
 * the peripheral subscriptions below for those signals.
 * ------------------------------------------------------------------- */

typedef struct avr_host_pin_t {
	uint8_t	dir;		/* 1 = output */
	uint8_t	out;		/* OUT register bit */
	uint8_t	in;			/* resolved input level (IN register bit) */
	uint8_t	pullup;		/* PINnCTRL.PULLUPEN */
	uint8_t	driven;		/* host is driving it via avr_host_pin_drive() */
	uint8_t	floating;	/* input, undriven, no pull-up */
} avr_host_pin_t;

/* Pin ids are port * 8 + bit (PA0 = 0, PB0 = 8, ...); this is one past the
 * highest id of a fitted port, so ids can be sparse and include bits a
 * package doesn't bond out. avr_host_pin_name() is NULL for unfitted. */
int
avr_host_pin_count(
		avr_host_t * h);
/* Returns pin id >= 0, or AVR_HOST_ENOENT if the pin isn't fitted. */
int
avr_host_pin_lookup(
		avr_host_t * h,
		const char * name);
const char *
avr_host_pin_name(
		avr_host_t * h,
		int pin);
int
avr_host_pin_get(
		avr_host_t * h,
		int pin,
		avr_host_pin_t * out);
/* Drive an input pin from outside (button, external part): 0 or 1.
 * Takes effect at the next instruction boundary. */
int
avr_host_pin_drive(
		avr_host_t * h,
		int pin,
		int level);
/* Stop driving. The pin follows its pull-up if enabled; otherwise the
 * port model keeps the last level (simavr has no floating-input model). */
int
avr_host_pin_release(
		avr_host_t * h,
		int pin);
/* Not implemented yet.
 * Analog input voltage for ADC/AC channels on this pin, in millivolts. */
int
avr_host_pin_set_mv(
		avr_host_t * h,
		int pin,
		uint32_t millivolts);

/* ---------------------------------------------------------------------
 * Serial (USART)
 * 'index' is the peripheral instance number (USART0 = 0).
 * ------------------------------------------------------------------- */

/* Queue bytes for the MCU to receive. They are fed into the USART's RX
 * FIFO as it has room (at the end of each batch), so long messages are
 * not lost to FIFO overflow. Bytes reaching a receiver that is disabled
 * (RXEN clear) are dropped by the USART model, as on hardware. */
int
avr_host_uart_send(
		avr_host_t * h,
		int index,
		const uint8_t * data,
		uint32_t len);

/* ---------------------------------------------------------------------
 * Event subscriptions
 * Each subscription returns an id > 0 that tags its events.
 * ------------------------------------------------------------------- */

typedef enum avr_host_event_type_t {
	AVR_HOST_EV_STATE = 1,	/* run state changed; a=state, b=stop reason, c=bp/watch id */
	AVR_HOST_EV_PIN,		/* a=pin id, b=new level (output of the pin) */
	AVR_HOST_EV_UART_TX,	/* a=USART index, b=byte */
	AVR_HOST_EV_WATCH,		/* a=addr, b=new value, c=old value (non-pausing watches) */
	AVR_HOST_EV_IRQ,		/* raw IRQ: a=irq index, b=value */
	AVR_HOST_EV_INTERRUPT,	/* vector taken: a=vector number, b=CPUINT level */
	AVR_HOST_EV_LOG,		/* firmware console / simavr log line: see avr_host_poll_text */
	AVR_HOST_EV_OVERFLOW,	/* a=number of events dropped */
} avr_host_event_type_t;

typedef struct avr_host_event_t {
	uint32_t	type;		/* avr_host_event_type_t */
	int32_t		sub;		/* subscription id, 0 for unsolicited (STATE, OVERFLOW) */
	uint64_t	cycle;		/* sim cycle at which it happened */
	uint32_t	a, b, c;
} avr_host_event_t;

/* Pin change events. If 'coalesce_cycles' > 0, at most one event per pin
 * is queued per that many cycles (last value wins, delivered once the
 * window expires or the instance stops), so a fast PWM pin doesn't flood
 * the GUI. 0 = every transition. Events from different subscriptions are
 * not strictly cycle-ordered in the queue; use the 'cycle' field. */
int
avr_host_subscribe_pin(
		avr_host_t * h,
		int pin,
		uint32_t coalesce_cycles);
int
avr_host_subscribe_uart(
		avr_host_t * h,
		int index);
/* One AVR_HOST_EV_INTERRUPT per vector taken (not per RETI); b is the CPUINT
 * level (0 LVL0 / classic, 1 LVL1, 2 NMI). */
int
avr_host_subscribe_interrupts(
		avr_host_t * h);
/* Escape hatch for anything else with an IRQ: an ioctl from the
 * peripheral headers (e.g. AVR_IOCTL_TCA_GETIRQ) plus irq index. These
 * ioctl values are a stable public contract; internal structs are not. */
int
avr_host_subscribe_irq(
		avr_host_t * h,
		uint32_t ioctl,
		int irq_index);
int
avr_host_unsubscribe(
		avr_host_t * h,
		int sub);

/* Drain up to 'max' events into 'out'; returns the count (0 if empty). */
int
avr_host_poll_events(
		avr_host_t * h,
		avr_host_event_t * out,
		int max);
/* Pollable fd, readable while events are pending. Do not read/close it. */
int
avr_host_event_fd(
		avr_host_t * h);
/* Not implemented yet (always returns 0).
 * Text for AVR_HOST_EV_LOG events, in order: copies the next line into
 * 'buf' (NUL-terminated, truncated to 'size'). Returns its full length,
 * or 0 if none. */
int
avr_host_poll_text(
		avr_host_t * h,
		char * buf,
		size_t size);

/* ---------------------------------------------------------------------
 * Tracing — not implemented yet
 * ------------------------------------------------------------------- */

/* VCD capture, same signal spec syntax as run_avr --add-trace
 * ("PA0=portpin@0x0/0x41"). Call add_trace before start_vcd. */
int
avr_host_vcd_add_trace(
		avr_host_t * h,
		const char * spec);
int
avr_host_vcd_start(
		avr_host_t * h,
		const char * path,
		uint32_t period_us);
int
avr_host_vcd_stop(
		avr_host_t * h);

/* ---------------------------------------------------------------------
 * Attached parts: SSD1306 OLED (128x64)
 * A native model of the controller (avr_ssd1306.c) wired to a TWI or SPI
 * bus plus pins, so the byte-by-byte ACKs happen inside the simulation. The
 * host only reads back the display state and draws it. Up to four per
 * instance; they live until avr_host_oled_detach() or avr_host_free(). Not modelled: scrolling, the
 * display offset (D3h), multiplex ratio.
 * ------------------------------------------------------------------- */

enum { AVR_HOST_OLED_TWI = 1, AVR_HOST_OLED_SPI = 2 };

typedef struct avr_host_oled_cfg_t {
	uint32_t	bus;		/* AVR_HOST_OLED_TWI / _SPI */
	uint8_t		instance;	/* TWI0/SPI0 = 0, TWI1/SPI1 = 1 ... */
	uint8_t		i2c_addr;	/* 7-bit: 0x3C or 0x3D (TWI only) */
	int16_t		cs_pin;		/* pin ids from avr_host_pin_lookup(); -1 = none. */
	int16_t		dc_pin;		/* SPI only; required there */
	int16_t		res_pin;	/* RES#: a falling edge resets the display */
} avr_host_oled_cfg_t;

typedef struct avr_host_oled_t {
	uint32_t	seq;		/* changes whenever anything below changes */
	uint8_t		on;			/* display ON (AFh); off after reset */
	uint8_t		all_on;		/* A5h: panel ignores RAM */
	uint8_t		inverse;	/* A7h */
	uint8_t		remap;		/* A1h: column 127 drives SEG0 */
	uint8_t		com_flip;	/* C8h: COM scan remapped */
	uint8_t		contrast;
	uint8_t		start_line;	/* RAM row shown on COM0, 0-63 */
	uint8_t		addr_mode;	/* 0 horizontal, 1 vertical, 2 page */
	uint8_t		vram[8][128];	/* [page][column]; bit 0 = top row of the page */
} avr_host_oled_t;

/* Returns an id >= 0, or ENOENT (bus or pin not on this device), EINVAL
 * (bad config), ENOMEM, ERANGE (too many). */
int
avr_host_oled_attach(
		avr_host_t * h,
		const avr_host_oled_cfg_t * cfg);
int
avr_host_oled_read(
		avr_host_t * h,
		int id,
		avr_host_oled_t * out);
/* Disconnect the part (its slot is reused). The part's wires stay in the
 * instance, now inert. */
int
avr_host_oled_detach(
		avr_host_t * h,
		int id);

/* ---------------------------------------------------------------------
 * Native escape hatch (UNSTABLE)
 * For attaching existing C parts (examples/parts: ssd1306, hd44780, ...)
 * to a host-managed instance. The returned avr_t* may only be touched
 * between avr_host_lock() / avr_host_unlock(), and IRQ notify callbacks
 * registered through it run on the sim thread. The lock is not recursive:
 * call no other avr_host_* function while holding it, and keep it short
 * (the sim thread is stalled meanwhile). Not covered by
 * AVR_HOST_API_VERSION.
 * ------------------------------------------------------------------- */

struct avr_t;

struct avr_t *
avr_host_raw_avr(
		avr_host_t * h);
void
avr_host_lock(
		avr_host_t * h);
void
avr_host_unlock(
		avr_host_t * h);

#ifdef __cplusplus
};
#endif

#endif /* __SIM_HOST_H__ */
