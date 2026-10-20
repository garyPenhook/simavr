/*
	sim_host.c

	Host-facing embedding API (see sim_host.h). Owns one simulation thread
	per instance, an instance lock that serialises every access to the
	avr_t, and a bounded event queue drained by the GUI.

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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <sched.h>
#include <pthread.h>
#ifndef _WIN32
#include <unistd.h>
#include <fcntl.h>
#endif

#include "sim_avr.h"
#include "sim_core.h"
#include "sim_io.h"
#include "sim_elf.h"
#include "sim_hex.h"
#include "avr_ioport.h"
#include "avr_port_modern.h"
#include "avr_uart.h"
#include "avr_usart_modern.h"
#include "avr_eeprom.h"
#include "avr_ssd1306.h"
#include "sim_host.h"

/* Only the *_isfull() accessors are used, to pace host -> MCU serial input. */
DEFINE_FIFO(uint16_t, uart_fifo);
DEFINE_FIFO(uint8_t, usart_rx_fifo);

extern avr_kind_t * avr_kind[];

#define HOST_MAX_BREAKS	64
#define HOST_MAX_WATCHES	32
#define HOST_EVQ_SIZE	4096
#define HOST_MAX_UARTS	8
#define HOST_MAX_PORTS	8		/* PORTA .. PORTH */
#define HOST_MAX_OLEDS	4
#define HOST_MAX_PINS	(HOST_MAX_PORTS * 8)

enum { SUB_PIN = 1, SUB_UART, SUB_IRQ, SUB_INT };

typedef struct host_sub_t {
	struct host_sub_t *	next;
	struct avr_host_t *	h;
	int					id;
	int					kind;
	avr_irq_t *			irq;
	uint32_t			arg;		/* pin id, USART index or IRQ index */
	uint32_t			coalesce;
	uint64_t			last_emit;
	int					pending;
	uint32_t			pending_value;
	uint64_t			pending_cycle;
} host_sub_t;

struct avr_host_t {
	avr_t *				avr;

	pthread_mutex_t		lock;		/* guards avr and everything below */
	pthread_cond_t		cond;		/* run-state changes, wakes the sim thread */
	/* Stop notification for avr_host_wait_stopped(), on its own lock: a
	 * waiter re-taking 'lock' after a timed wait would not be counted in
	 * 'waiters' and could lose it to the sim thread for seconds. */
	pthread_mutex_t		stop_lock;
	pthread_cond_t		stop_cond;
	int					running;	/* mirror of mode == RUNNING, under stop_lock */
	pthread_t			thread;
	int					threaded;
	int					quit;
	int					pause_req;	/* atomic: honoured at the next instruction */
	int					waiters;	/* atomic: threads queued on 'lock' */

	int					mode;		/* PAUSED, RUNNING, DONE or CRASHED */
	uint32_t			step_left;
	int					cycle_limit;
	uint64_t			cycle_target;

	double				speed;
	uint32_t			batch;
	uint64_t			pace_cycle;
	struct timespec		pace_wall;

	int					next_id;
	int					resume_pc_valid;	/* last stop left PC at resume_pc */
	uint32_t			resume_pc;
	int					skip_break;	/* this run's first instruction skips breaks */
	int					nbreaks;
	uint32_t			break_pc[HOST_MAX_BREAKS];
	int					break_id[HOST_MAX_BREAKS];
	int					nwatches;
	struct {
		int				id;
		uint16_t		addr, len;
		uint32_t		flags;
		uint8_t *		prev;		/* PERIPH: last seen bytes of [addr, addr+len) */
	}					watch[HOST_MAX_WATCHES];
	int					nperiph;	/* watches with AVR_HOST_WATCH_PERIPH */
	int					in_run;		/* inside avr_run(): host peeks/pokes must not hit watches */
	int					watch_hit;	/* id of the watch that fired this instruction */
	uint16_t			watch_addr;

	host_sub_t *		subs;

	avr_ssd1306_t *		oled[HOST_MAX_OLEDS];

	uint8_t				ports;		/* bitmask of fitted PORTA.. */
	uint8_t				ext_mask[HOST_MAX_PORTS];	/* host-driven pins */
	uint8_t				ext_value[HOST_MAX_PORTS];
	uint8_t				fw_ext_mask[HOST_MAX_PORTS];	/* .mmcu external pulls */
	uint8_t				fw_ext_value[HOST_MAX_PORTS];
	char				pin_name[HOST_MAX_PINS][4];

	struct {
		avr_io_t *		io;
		uint8_t *		buf;
		uint32_t		len, size;
	} uart[HOST_MAX_UARTS];

	pthread_mutex_t		ev_lock;	/* guards the event queue only */
	avr_host_event_t	evq[HOST_EVQ_SIZE];
	uint32_t			ev_head, ev_count, ev_dropped;
	int					ev_pipe[2];

};

/* Last failure on the calling thread (errno-style), so concurrent callers
 * of one instance never see each other's messages. */
static __thread struct {
	int		code;
	char	msg[256];
} host_err;

static void host_sub_notify(struct avr_irq_t * irq, uint32_t value,
		void * param);
static int host_load_common(avr_host_t * h, elf_firmware_t * f);

/* ---------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------- */

static int
host_error(
		avr_host_t * h,
		int code,
		const char * fmt,
		...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(host_err.msg, sizeof(host_err.msg), fmt, ap);
	va_end(ap);
	host_err.code = code;
	return code;
}

static void
host_lock(
		avr_host_t * h)
{
	__atomic_add_fetch(&h->waiters, 1, __ATOMIC_SEQ_CST);
	pthread_mutex_lock(&h->lock);
	__atomic_sub_fetch(&h->waiters, 1, __ATOMIC_SEQ_CST);
}

static void
host_unlock(
		avr_host_t * h)
{
	pthread_mutex_unlock(&h->lock);
}

static avr_host_state_t
host_public_state(
		avr_host_t * h)
{
	if (h->mode == AVR_HOST_RUNNING && h->avr->state == cpu_Sleeping)
		return AVR_HOST_SLEEPING;
	return h->mode;
}

static void
host_push(
		avr_host_t * h,
		uint32_t type,
		int sub,
		uint64_t cycle,
		uint32_t a,
		uint32_t b,
		uint32_t c)
{
	pthread_mutex_lock(&h->ev_lock);
	if (h->ev_count == HOST_EVQ_SIZE) {
		h->ev_dropped++;
	} else {
		avr_host_event_t *e =
			&h->evq[(h->ev_head + h->ev_count) % HOST_EVQ_SIZE];
		e->type = type;
		e->sub = sub;
		e->cycle = cycle;
		e->a = a;
		e->b = b;
		e->c = c;
		if (h->ev_count++ == 0 && h->ev_pipe[1] >= 0) {
#ifndef _WIN32
			uint8_t one = 1;
			if (write(h->ev_pipe[1], &one, 1) < 0) {
				/* full pipe still reads as "pending"; nothing to do */
			}
#endif
		}
	}
	pthread_mutex_unlock(&h->ev_lock);
}

/* Emit coalesced pin values whose window has expired; 'force' (on stop)
 * emits them all so a paused GUI shows the final pin state. */
static void
host_flush_coalesced(
		avr_host_t * h,
		int force)
{
	for (host_sub_t * s = h->subs; s; s = s->next) {
		if (!s->pending)
			continue;
		if (!force && h->avr->cycle - s->last_emit < s->coalesce)
			continue;
		s->pending = 0;
		s->last_emit = s->pending_cycle;
		host_push(h, AVR_HOST_EV_PIN, s->id, s->pending_cycle,
				  s->arg, s->pending_value, 0);
	}
}

/* Called with 'lock' held after every change of h->mode. */
static void
host_publish_mode(
		avr_host_t * h)
{
	pthread_mutex_lock(&h->stop_lock);
	h->running = h->mode == AVR_HOST_RUNNING;
	pthread_cond_broadcast(&h->stop_cond);
	pthread_mutex_unlock(&h->stop_lock);
}

static void
host_set_mode(
		avr_host_t * h,
		int mode,
		int reason,
		int id)
{
	h->mode = mode;
	if (mode != AVR_HOST_RUNNING) {
		h->step_left = 0;
		h->cycle_limit = 0;
		/* Pin values first, so a GUI that stops polling at STATE is current. */
		host_flush_coalesced(h, 1);
		h->resume_pc_valid = mode == AVR_HOST_PAUSED;
		h->resume_pc = h->avr->pc;
	}
	host_push(h, AVR_HOST_EV_STATE, 0, h->avr->cycle,
			  host_public_state(h), reason, id);
	/* Last: wait_stopped() must not return before the STATE event (and the
	 * flushed pin values) are queued. */
	host_publish_mode(h);
	pthread_cond_broadcast(&h->cond);
}

static void
host_pace_rebase(
		avr_host_t * h)
{
	h->pace_cycle = h->avr->cycle;
	clock_gettime(CLOCK_MONOTONIC, &h->pace_wall);
}

static uint32_t
host_batch(
		avr_host_t * h)
{
	if (h->batch)
		return h->batch;
	uint32_t b = h->avr->frequency / 1000;
	return b < 1000 ? 1000 : b;
}

/* The sim thread does its own pacing; never block inside avr_run(). */
static void
host_sleep_cb(
		avr_t * avr,
		avr_cycle_count_t how_long)
{
}

static int
host_parse_pin(
		avr_host_t * h,
		int pin,
		char * port,
		int * bit)
{
	if (pin < 0 || pin >= HOST_MAX_PINS || !(h->ports & (1 << (pin / 8))))
		return host_error(h, AVR_HOST_ENOENT, "pin %d not fitted", pin);
	*port = 'A' + pin / 8;
	*bit = pin % 8;
	return 0;
}

static avr_port_modern_t *
host_modern_port(
		avr_t * avr,
		char name)
{
	for (avr_io_t * io = avr->io_port; io; io = io->next)
		if (io->kind && !strcmp(io->kind, "port_modern") &&
				((avr_port_modern_t *)io)->name == name)
			return (avr_port_modern_t *)io;
	return NULL;
}

/* Raise a pin IRQ even if its value is unchanged, so the port model
 * latches it into IN (avr_reset() zeroes IN but not the IRQ values, and
 * the classic port doesn't re-evaluate pins on SET_EXTERNAL). */
static void
host_force_pin(
		avr_host_t * h,
		int port_index,
		int bit,
		int level)
{
	avr_irq_t * irq = avr_io_getirq(h->avr,
			AVR_IOCTL_IOPORT_GETIRQ('A' + port_index), bit);
	if (!irq)
		return;
	uint8_t flags = irq->flags;
	irq->flags &= ~IRQ_FLAG_FILTERED;
	avr_raise_irq(irq, level);
	irq->flags = flags;
}

/*
 * Push host drive + firmware-declared pulls of one port into the port
 * model, then latch the resulting level of each input pin in 'touched'.
 */
static int
host_apply_external(
		avr_host_t * h,
		int port_index,
		uint8_t touched)
{
	avr_t * avr = h->avr;
	char name = 'A' + port_index;
	uint8_t hm = h->ext_mask[port_index], fm = h->fw_ext_mask[port_index];
	avr_ioport_external_t e = {
		.name = name,
		.mask = hm | fm,
		.value = (h->ext_value[port_index] & hm) |
				 (h->fw_ext_value[port_index] & fm & ~hm),
	};
	if (avr_ioctl(avr, AVR_IOCTL_IOPORT_SET_EXTERNAL(name), &e) < 0)
		return host_error(h, AVR_HOST_ENOTSUP,
				"PORT%c does not accept external drive", name);

	avr_ioport_state_t st;
	if (!touched || avr_ioctl(avr, AVR_IOCTL_IOPORT_GETSTATE(name), &st) != 0)
		return 0;
	avr_port_modern_t * mp = host_modern_port(avr, name);
	for (int b = 0; b < 8; b++) {
		uint8_t m = 1 << b;
		if (!(touched & m) || (st.ddr & m))
			continue;			/* outputs are driven by the port itself */
		if (e.mask & m)
			host_force_pin(h, port_index, b, !!(e.value & m));
		else if (mp ? (avr->data[mp->r_pinctrl + b] & PINCTRL_PULLUPEN) :
					  (st.port & m))
			host_force_pin(h, port_index, b, 1);	/* back to its pull-up */
	}
	return 0;
}

static void
host_after_reset(
		avr_host_t * h)
{
	for (int pi = 0; pi < HOST_MAX_PORTS; pi++)
		if (h->ext_mask[pi] | h->fw_ext_mask[pi])
			host_apply_external(h, pi, h->ext_mask[pi] | h->fw_ext_mask[pi]);
	if (h->mode == AVR_HOST_DONE || h->mode == AVR_HOST_CRASHED)
		host_set_mode(h, AVR_HOST_PAUSED, AVR_HOST_STOP_NONE, 0);
	h->resume_pc_valid = 0;		/* a breakpoint at the reset vector must hit */
}

static int
host_uart_full(
		avr_io_t * io)
{
	if (!strcmp(io->kind, "usart_modern"))
		return usart_rx_fifo_isfull(&((avr_usart_modern_t *)io)->rx);
	return uart_fifo_isfull(&((avr_uart_t *)io)->input);
}

static void
host_feed_uarts(
		avr_host_t * h)
{
	for (int i = 0; i < HOST_MAX_UARTS; i++) {
		uint32_t n = 0;
		if (!h->uart[i].len)
			continue;
		while (n < h->uart[i].len && !host_uart_full(h->uart[i].io))
			avr_raise_irq(h->uart[i].io->irq + UART_IRQ_INPUT,
						  h->uart[i].buf[n++]);
		memmove(h->uart[i].buf, h->uart[i].buf + n, h->uart[i].len - n);
		h->uart[i].len -= n;
	}
}

/*
 * Execute until the run mode leaves RUNNING or 'budget' cycles elapse.
 * Called with the lock held, from the sim thread or (manual mode) the
 * caller's thread.
 */
static void
host_exec(
		avr_host_t * h,
		uint32_t budget)
{
	avr_t * avr = h->avr;
	avr_cycle_count_t start = avr->cycle;

	/* Host pokes and earlier stops may have changed watched bytes: re-baseline,
	 * so only changes made while running trigger a PERIPH watch. */
	for (int i = 0; h->nperiph && i < h->nwatches; i++)
		if (h->watch[i].prev)
			memcpy(h->watch[i].prev, h->avr->data + h->watch[i].addr, h->watch[i].len);

	while (h->mode == AVR_HOST_RUNNING && avr->cycle - start < budget) {
		if (__atomic_exchange_n(&h->pause_req, 0, __ATOMIC_SEQ_CST)) {
			host_set_mode(h, AVR_HOST_PAUSED, AVR_HOST_STOP_USER, 0);
			break;
		}
		/* Breakpoints fire before the instruction at PC runs, only when one
		 * is about to run (not while asleep), and not on the instruction a
		 * paused instance resumes from. */
		int executed = avr->state == cpu_Running;
		if (executed && h->nbreaks && !h->skip_break) {
			int hit = 0;
			for (int i = 0; i < h->nbreaks && !hit; i++)
				if (avr->pc == h->break_pc[i]) {
					host_set_mode(h, AVR_HOST_PAUSED,
								  AVR_HOST_STOP_BREAKPOINT, h->break_id[i]);
					hit = 1;
				}
			if (hit)
				break;
		}
		h->in_run = 1;
		int st = avr_run(avr);
		h->in_run = 0;
		if (executed)
			h->skip_break = 0;
		for (int i = 0; h->nperiph && !h->watch_hit && i < h->nwatches; i++) {
			uint8_t * cur = avr->data + h->watch[i].addr;
			if (h->watch[i].prev && memcmp(h->watch[i].prev, cur, h->watch[i].len)) {
				memcpy(h->watch[i].prev, cur, h->watch[i].len);
				h->watch_hit = h->watch[i].id;
			}
		}
		if (h->watch_hit) {
			int id = h->watch_hit;
			h->watch_hit = 0;
			host_set_mode(h, AVR_HOST_PAUSED, AVR_HOST_STOP_WATCHPOINT, id);
			break;
		}
		if (st == cpu_Done) {
			host_set_mode(h, AVR_HOST_DONE, AVR_HOST_STOP_DONE, 0);
			break;
		}
		if (st == cpu_Crashed || st == cpu_Stopped) {
			host_set_mode(h, AVR_HOST_CRASHED, AVR_HOST_STOP_CRASH, 0);
			break;
		}
		if (executed && h->step_left && --h->step_left == 0) {
			host_set_mode(h, AVR_HOST_PAUSED, AVR_HOST_STOP_STEP, 0);
			break;
		}
		if (h->cycle_limit && avr->cycle >= h->cycle_target) {
			host_set_mode(h, AVR_HOST_PAUSED, AVR_HOST_STOP_STEP, 0);
			break;
		}
	}
	host_flush_coalesced(h, 0);
	host_feed_uarts(h);
}

static void
host_pace(
		avr_host_t * h)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	double sim_ns = (double)(h->avr->cycle - h->pace_cycle) * 1e9 /
					h->avr->frequency;
	double wall_ns = (now.tv_sec - h->pace_wall.tv_sec) * 1e9 +
					 (now.tv_nsec - h->pace_wall.tv_nsec);
	double ahead = sim_ns / h->speed - wall_ns;

	if (ahead < -100e6) {		/* fell >100 ms behind: don't try to catch up */
		host_pace_rebase(h);
		return;
	}
	if (ahead <= 0)
		return;
	if (ahead > 50e6)
		ahead = 50e6;
	struct timespec d = { 0, (long)ahead };
	host_unlock(h);
	nanosleep(&d, NULL);
	host_lock(h);
}

static void *
host_thread(
		void * param)
{
	avr_host_t * h = param;

	host_lock(h);
	while (!h->quit) {
		if (h->mode != AVR_HOST_RUNNING) {
			pthread_cond_wait(&h->cond, &h->lock);
			continue;
		}
		host_exec(h, host_batch(h));
		if (h->mode == AVR_HOST_RUNNING && h->speed > 0)
			host_pace(h);
		/* Hand the lock to queued accessors before the next batch, but
		 * bounded, so a GUI polling in a tight loop can't starve us. */
		host_unlock(h);
		for (int i = 0; i < 64 && __atomic_load_n(&h->waiters,
				__ATOMIC_SEQ_CST); i++)
			sched_yield();
		pthread_mutex_lock(&h->lock);
	}
	host_unlock(h);
	return NULL;
}

/* ---------------------------------------------------------------------
 * Discovery / lifecycle
 * ------------------------------------------------------------------- */

int
avr_host_api_version(void)
{
	return AVR_HOST_API_VERSION;
}

int
avr_host_core_count(void)
{
	int n = 0;
	while (avr_kind[n])
		n++;
	return n;
}

const char *
avr_host_core_name(
		int index)
{
	if (index < 0 || index >= avr_host_core_count())
		return NULL;
	return avr_kind[index]->names[0];
}

static avr_host_t *
host_wrap(
		avr_t * avr)
{
	avr_host_t * h = calloc(1, sizeof(*h));
	if (!h) {
		host_error(NULL, AVR_HOST_ENOMEM, "out of memory");
		return NULL;
	}
	h->avr = avr;
	h->mode = AVR_HOST_PAUSED;
	h->next_id = 1;
	h->ev_pipe[0] = h->ev_pipe[1] = -1;
	pthread_mutex_init(&h->lock, NULL);
	pthread_mutex_init(&h->ev_lock, NULL);
	pthread_cond_init(&h->cond, NULL);
	pthread_mutex_init(&h->stop_lock, NULL);
	pthread_condattr_t ca;
	pthread_condattr_init(&ca);
	pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);	/* immune to clock jumps */
	pthread_cond_init(&h->stop_cond, &ca);
	pthread_condattr_destroy(&ca);
#ifndef _WIN32
	if (pipe(h->ev_pipe) == 0) {
		for (int i = 0; i < 2; i++) {
			fcntl(h->ev_pipe[i], F_SETFL,
				  fcntl(h->ev_pipe[i], F_GETFL) | O_NONBLOCK);
			fcntl(h->ev_pipe[i], F_SETFD, FD_CLOEXEC);
		}
	} else
		h->ev_pipe[0] = h->ev_pipe[1] = -1;
#endif
	avr->sleep = host_sleep_cb;

	for (int pi = 0; pi < HOST_MAX_PORTS; pi++) {
		avr_ioport_state_t st;
		if (avr_ioctl(avr, AVR_IOCTL_IOPORT_GETSTATE('A' + pi), &st) != 0)
			continue;
		h->ports |= 1 << pi;
		for (int b = 0; b < 8; b++)
			snprintf(h->pin_name[pi * 8 + b], sizeof(h->pin_name[0]),
					 "P%c%d", 'A' + pi, b);
	}
	for (avr_io_t * io = avr->io_port; io; io = io->next) {
		char name;
		if (!io->kind)
			continue;
		if (!strcmp(io->kind, "usart_modern"))
			name = ((avr_usart_modern_t *)io)->name;
		else if (!strcmp(io->kind, "uart"))
			name = ((avr_uart_t *)io)->name;
		else
			continue;
		if (name >= '0' && name < '0' + HOST_MAX_UARTS && io->irq)
			h->uart[name - '0'].io = io;
	}
	return h;
}

avr_host_t *
avr_host_new(
		const char * mmcu,
		uint32_t freq_hz)
{
	if (!mmcu) {
		host_error(NULL, AVR_HOST_EINVAL, "no mmcu given");
		return NULL;
	}
	avr_t * avr = avr_make_mcu_by_name(mmcu);
	if (!avr) {
		host_error(NULL, AVR_HOST_ENOENT, "unknown core '%s'", mmcu);
		return NULL;
	}
	if (freq_hz)
		avr->frequency = freq_hz;
	avr_init(avr);
	avr_host_t * h = host_wrap(avr);
	if (!h) {
		avr_terminate(avr);
		free(avr);
	}
	return h;
}

static void
host_free_firmware(
		elf_firmware_t * f)
{
	fw_chunk_t * c = f->chunks;		/* only left over on failed loads */
	while (c) {
		fw_chunk_t * n = c->next;
		free(c);
		c = n;
	}
#if ELF_SYMBOLS && !CONFIG_SIMAVR_TRACE
	/* With tracing on, avr_load_firmware() keeps pointers into these. */
	for (uint32_t i = 0; i < f->symbolcount; i++)
		free(f->symbol[i]);
	free(f->symbol);
#endif
	free(f->file_name);
	free(f);
}

avr_host_t *
avr_host_new_from_elf(
		const char * path,
		const char * fallback_mmcu,
		uint32_t fallback_freq_hz)
{
	elf_firmware_t * f = calloc(1, sizeof(*f));
	if (!f) {
		host_error(NULL, AVR_HOST_ENOMEM, "out of memory");
		return NULL;
	}
	if (!path || elf_read_firmware(path, f) != 0) {
		host_error(NULL, AVR_HOST_EIO, "cannot read ELF '%s'",
				   path ? path : "(null)");
		host_free_firmware(f);
		return NULL;
	}
	const char * mmcu = f->mmcu[0] ? f->mmcu : fallback_mmcu;
	if (!mmcu) {
		host_error(NULL, AVR_HOST_EINVAL,
				   "'%s' has no .mmcu section and no fallback mmcu", path);
		host_free_firmware(f);
		return NULL;
	}
	avr_host_t * h = avr_host_new(mmcu,
			f->frequency ? f->frequency : fallback_freq_hz);
	if (h && host_load_common(h, f) != 0) {
		avr_host_free(h);			/* error already recorded */
		h = NULL;
	}
	host_free_firmware(f);
	return h;
}

void
avr_host_free(
		avr_host_t * h)
{
	if (!h)
		return;
	if (h->threaded) {
		host_lock(h);
		h->quit = 1;
		pthread_cond_broadcast(&h->cond);
		host_unlock(h);
		pthread_join(h->thread, NULL);
	}
	while (h->subs) {
		host_sub_t * s = h->subs;
		h->subs = s->next;
		avr_irq_unregister_notify(s->irq, host_sub_notify, s);
		free(s);
	}
	for (int i = 0; i < HOST_MAX_OLEDS; i++)
		avr_ssd1306_free(h->oled[i]);
	avr_terminate(h->avr);
	free(h->avr);
	for (int i = 0; i < HOST_MAX_UARTS; i++)
		free(h->uart[i].buf);
	for (int i = 0; i < h->nwatches; i++)
		free(h->watch[i].prev);
#ifndef _WIN32
	if (h->ev_pipe[0] >= 0) {
		close(h->ev_pipe[0]);
		close(h->ev_pipe[1]);
	}
#endif
	pthread_cond_destroy(&h->cond);
	pthread_cond_destroy(&h->stop_cond);
	pthread_mutex_destroy(&h->stop_lock);
	pthread_mutex_destroy(&h->ev_lock);
	pthread_mutex_destroy(&h->lock);
	free(h);
}

const char *
avr_host_last_error(
		avr_host_t * h)
{
	return host_err.msg;
}

int
avr_host_last_error_code(
		avr_host_t * h)
{
	return host_err.code;
}

const char *
avr_host_mmcu(
		avr_host_t * h)
{
	return h->avr->mmcu;
}

uint32_t
avr_host_frequency(
		avr_host_t * h)
{
	return h->avr->frequency;
}

/* ---------------------------------------------------------------------
 * Firmware
 * ------------------------------------------------------------------- */

/* EEPROM data offset of a chunk, or -1 if it isn't EEPROM data. ELF
 * chunks are still unclassified (segment address), HEX ones typed. */
static int64_t
host_eeprom_offset(
		fw_chunk_t * c)
{
	if (c->type == EEPROM)
		return c->addr;
	if (c->type == UNKNOWN && c->addr >= AVR_SEGMENT_OFFSET_EEPROM &&
			c->addr < AVR_SEGMENT_OFFSET_FUSES)
		return c->addr - AVR_SEGMENT_OFFSET_EEPROM;
	return -1;
}

/*
 * Load a parsed firmware, then reset. Modern EEPROM lives in the data
 * space and has no AVR_IOCTL_EEPROM_SET handler, so its chunks are copied
 * here instead of by avr_load_firmware() (which would drop them).
 */
static int
host_load_common(
		avr_host_t * h,
		elf_firmware_t * f)
{
	avr_t * avr = h->avr;
	int modern_ee = (avr->arch.flags & AVR_ARCH_F_MODERN) &&
					avr->arch.persist_end;
	uint32_t ee_size = modern_ee ?
			avr->arch.persist_end - avr->arch.persist_start + 1 : 0;

	if (modern_ee)
		for (fw_chunk_t * c = f->chunks; c; c = c->next) {
			int64_t off = host_eeprom_offset(c);
			if (off >= 0 && off + c->size > ee_size)
				return host_error(h, AVR_HOST_ERANGE,
						"EEPROM data %#x+%u exceeds %u bytes",
						(unsigned)off, c->size, ee_size);
		}

	memset(avr->flash, 0xff, avr->flashend + 1);
	for (fw_chunk_t ** pp = &f->chunks; modern_ee && *pp; ) {
		fw_chunk_t * c = *pp;
		int64_t off = host_eeprom_offset(c);
		if (off < 0) {
			pp = &c->next;
			continue;
		}
		memcpy(avr->data + avr->arch.persist_start + off, c->data, c->size);
		*pp = c->next;
		free(c);
	}
	memset(h->fw_ext_mask, 0, sizeof(h->fw_ext_mask));
	memset(h->fw_ext_value, 0, sizeof(h->fw_ext_value));
	for (int i = 0; i < 8 && f->external_state[i].port; i++) {
		int pi = f->external_state[i].port - 'A';
		if (pi < 0 || pi >= HOST_MAX_PORTS)
			continue;
		h->fw_ext_mask[pi] = f->external_state[i].mask;
		h->fw_ext_value[pi] = f->external_state[i].value;
	}

	avr_load_firmware(avr, f);
	f->chunks = NULL;			/* consumed */
	f->dwarf_file = NULL;		/* freed by avr_load_firmware() */
	avr->sleep = host_sleep_cb;
	avr_reset(avr);
	host_after_reset(h);
	if (f->flashbase)			/* bootloader image: start there, like run_avr */
		avr->pc = f->flashbase;
	return 0;
}

int
avr_host_load_elf(
		avr_host_t * h,
		const char * path)
{
	int res;
	elf_firmware_t * f = calloc(1, sizeof(*f));
	if (!f)
		return host_error(h, AVR_HOST_ENOMEM, "out of memory");
	host_lock(h);
	if (h->mode == AVR_HOST_RUNNING)
		res = host_error(h, AVR_HOST_ESTATE, "pause before loading firmware");
	else if (!path || elf_read_firmware(path, f) != 0)
		res = host_error(h, AVR_HOST_EIO, "cannot read ELF '%s'",
						 path ? path : "(null)");
	else if (f->mmcu[0] && strcmp(f->mmcu, h->avr->mmcu))
		res = host_error(h, AVR_HOST_EINVAL, "'%s' is built for %s, not %s",
						 path, f->mmcu, h->avr->mmcu);
	else
		res = host_load_common(h, f);
	host_unlock(h);
	host_free_firmware(f);
	return res;
}

int
avr_host_load_hex(
		avr_host_t * h,
		const char * path)
{
	int res;
	fw_chunk_t * chunks = NULL;
	elf_firmware_t * f = calloc(1, sizeof(*f));
	if (!f)
		return host_error(h, AVR_HOST_ENOMEM, "out of memory");
	host_lock(h);
	if (h->mode == AVR_HOST_RUNNING) {
		res = host_error(h, AVR_HOST_ESTATE, "pause before loading firmware");
		goto out;
	}
	if (!path || read_ihex_chunks(path, &chunks) != 0 || !chunks) {
		f->chunks = chunks;		/* partial list, freed below */
		res = host_error(h, AVR_HOST_EIO, "cannot read HEX '%s'",
						 path ? path : "(null)");
		goto out;
	}
	/* Same classification as sim_setup_firmware() with a zero load base. */
	for (fw_chunk_t * c = chunks; c; c = c->next) {
		if (c->addr < 1024 * 1024)
			c->type = FLASH;
		else if (c->addr >= AVR_SEGMENT_OFFSET_EEPROM) {
			c->addr -= AVR_SEGMENT_OFFSET_EEPROM;
			c->type = EEPROM;
		} else
			c->type = UNKNOWN;
	}
	f->chunks = chunks;
	res = host_load_common(h, f);
out:
	host_unlock(h);
	host_free_firmware(f);
	return res;
}

int
avr_host_load_flash(
		avr_host_t * h,
		const uint8_t * data,
		uint32_t size,
		uint32_t addr)
{
	int res = 0;
	host_lock(h);
	if (h->mode == AVR_HOST_RUNNING)
		res = host_error(h, AVR_HOST_ESTATE, "pause before loading firmware");
	else if (!data || (uint64_t)addr + size > (uint64_t)h->avr->flashend + 1)
		res = host_error(h, AVR_HOST_ERANGE, "image does not fit in flash");
	else {
		memcpy(h->avr->flash + addr, data, size);
		avr_reset(h->avr);
		host_after_reset(h);
	}
	host_unlock(h);
	return res;
}

/* ---------------------------------------------------------------------
 * Run control
 * ------------------------------------------------------------------- */

int
avr_host_start(
		avr_host_t * h)
{
	int res = 0;
	host_lock(h);
	if (h->threaded)
		res = host_error(h, AVR_HOST_ESTATE, "already started");
	else if (pthread_create(&h->thread, NULL, host_thread, h) != 0)
		res = host_error(h, AVR_HOST_ENOMEM, "cannot create sim thread");
	else
		h->threaded = 1;
	host_unlock(h);
	return res;
}

static int
host_can_run(
		avr_host_t * h)
{
	if (h->mode == AVR_HOST_RUNNING)
		return host_error(h, AVR_HOST_ESTATE, "already running");
	if (h->mode == AVR_HOST_DONE || h->mode == AVR_HOST_CRASHED)
		return host_error(h, AVR_HOST_ESTATE,
						  "firmware has stopped; reset first");
	return 0;
}

/* Kick a prepared step/run: wake the sim thread, or run it here. */
static void
host_go(
		avr_host_t * h)
{
	__atomic_store_n(&h->pause_req, 0, __ATOMIC_SEQ_CST);
	/* Resuming from where we stopped (e.g. on a breakpoint) must execute
	 * that instruction rather than hit the same breakpoint again. */
	h->skip_break = h->resume_pc_valid && h->avr->pc == h->resume_pc;
	h->mode = AVR_HOST_RUNNING;
	host_publish_mode(h);
	host_pace_rebase(h);
	if (h->threaded) {
		pthread_cond_broadcast(&h->cond);
		return;
	}
	while (h->mode == AVR_HOST_RUNNING)
		host_exec(h, host_batch(h));
}

int
avr_host_resume(
		avr_host_t * h)
{
	int res;
	host_lock(h);
	if (!h->threaded)
		res = host_error(h, AVR_HOST_ESTATE,
						 "resume needs avr_host_start(); use run_cycles");
	else if ((res = host_can_run(h)) == 0) {
		host_go(h);
		host_push(h, AVR_HOST_EV_STATE, 0, h->avr->cycle,
				  AVR_HOST_RUNNING, AVR_HOST_STOP_NONE, 0);
	}
	host_unlock(h);
	return res;
}

int
avr_host_pause(
		avr_host_t * h)
{
	/* No lock: the sim thread may be mid-batch; it checks this every
	 * instruction. A pause while not running is a no-op (cleared on go). */
	__atomic_store_n(&h->pause_req, 1, __ATOMIC_SEQ_CST);
	return 0;
}

int
avr_host_step(
		avr_host_t * h,
		uint32_t count)
{
	int res;
	if (!count)
		return host_error(h, AVR_HOST_EINVAL, "step count is 0");
	host_lock(h);
	if ((res = host_can_run(h)) == 0) {
		h->step_left = count;
		host_go(h);
	}
	host_unlock(h);
	return res;
}

int
avr_host_run_cycles(
		avr_host_t * h,
		uint64_t cycles)
{
	int res;
	if (!cycles)
		return host_error(h, AVR_HOST_EINVAL, "cycle count is 0");
	host_lock(h);
	if ((res = host_can_run(h)) == 0) {
		h->cycle_limit = 1;
		h->cycle_target = h->avr->cycle + cycles;
		host_go(h);
	}
	host_unlock(h);
	return res;
}

int
avr_host_reset(
		avr_host_t * h)
{
	host_lock(h);
	/* avr_reset() zeroes the cycle counter; keep a pending run_cycles()
	 * budget relative to it. */
	uint64_t left = h->cycle_target > h->avr->cycle ?
					h->cycle_target - h->avr->cycle : 0;
	avr_reset(h->avr);
	h->cycle_target = h->avr->cycle + left;
	host_after_reset(h);
	host_pace_rebase(h);
	host_unlock(h);
	return 0;
}

avr_host_state_t
avr_host_get_state(
		avr_host_t * h)
{
	host_lock(h);
	avr_host_state_t s = host_public_state(h);
	host_unlock(h);
	return s;
}

int
avr_host_wait_stopped(
		avr_host_t * h,
		int timeout_ms)
{
	int res = 0;
	struct timespec deadline;

	clock_gettime(CLOCK_MONOTONIC, &deadline);
	if (timeout_ms > 0) {
		deadline.tv_sec += timeout_ms / 1000;
		deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
		if (deadline.tv_nsec >= 1000000000L) {
			deadline.tv_sec++;
			deadline.tv_nsec -= 1000000000L;
		}
	}
	pthread_mutex_lock(&h->stop_lock);
	while (h->running && res == 0) {
		if (timeout_ms < 0)
			pthread_cond_wait(&h->stop_cond, &h->stop_lock);
		else if (timeout_ms == 0 ||
				pthread_cond_timedwait(&h->stop_cond, &h->stop_lock,
									   &deadline) == ETIMEDOUT)
			res = h->running ? AVR_HOST_ESTATE : 0;
	}
	pthread_mutex_unlock(&h->stop_lock);
	return res ? host_error(h, res, "still running") : 0;
}

int
avr_host_set_speed(
		avr_host_t * h,
		double ratio)
{
	if (!(ratio >= 0))
		return host_error(h, AVR_HOST_EINVAL, "speed must be >= 0");
	host_lock(h);
	h->speed = ratio;
	host_pace_rebase(h);
	host_unlock(h);
	return 0;
}

int
avr_host_set_batch(
		avr_host_t * h,
		uint32_t cycles)
{
	host_lock(h);
	h->batch = cycles;		/* 0 = default */
	host_unlock(h);
	return 0;
}

uint64_t
avr_host_cycle(
		avr_host_t * h)
{
	host_lock(h);
	uint64_t c = h->avr->cycle;
	host_unlock(h);
	return c;
}

uint64_t
avr_host_time_ns(
		avr_host_t * h)
{
	host_lock(h);
	uint64_t ns = (uint64_t)((double)h->avr->cycle * 1e9 / h->avr->frequency);
	host_unlock(h);
	return ns;
}

/* ---------------------------------------------------------------------
 * CPU state
 * ------------------------------------------------------------------- */

int
avr_host_get_cpu(
		avr_host_t * h,
		avr_host_cpu_t * out)
{
	int res = 0;
	host_lock(h);
	if (h->mode == AVR_HOST_RUNNING)
		res = host_error(h, AVR_HOST_ESTATE, "pause first");
	else {
		avr_t * avr = h->avr;
		out->pc = avr->pc;
		out->sp = _avr_sp_get(avr);
		READ_SREG_INTO(avr, out->sreg);
		memcpy(out->r, avr->data, 32);
		out->cycle = avr->cycle;
	}
	host_unlock(h);
	return res;
}

int
avr_host_sample_cpu(
		avr_host_t * h,
		avr_host_cpu_t * out)
{
	/* The sim thread runs a whole batch under the lock, so between batches
	 * this is a consistent snapshot, in any state. */
	host_lock(h);
	avr_t * avr = h->avr;
	out->pc = avr->pc;
	out->sp = _avr_sp_get(avr);
	READ_SREG_INTO(avr, out->sreg);
	memcpy(out->r, avr->data, 32);
	out->cycle = avr->cycle;
	host_unlock(h);
	return 0;
}

int
avr_host_pending_interrupts(
		avr_host_t * h,
		uint8_t * vectors,
		uint8_t * levels,
		int max)
{
	int res = 0;
	host_lock(h);
	if (h->mode == AVR_HOST_RUNNING)
		res = host_error(h, AVR_HOST_ESTATE, "pause first");
	else {
		avr_t * avr = h->avr;
		avr_int_table_t * t = &avr->interrupts;
		struct { int vec, level, rank; } q[256];
		for (int i = 0; i < t->vector_count; i++) {
			avr_int_vector_t * v = t->vector[i];
			/* the dispatcher skips a pending vector whose enable was cleared */
			if (!v->pending || !avr_regbit_get(avr, v->enable))
				continue;
			int dup = 0;
			for (int j = 0; j < res; j++)
				dup |= q[j].vec == v->vector;
			if (dup)
				continue;
			q[res].vec = v->vector;
			avr_interrupt_dispatch_key(avr, v, &q[res].level, &q[res].rank);
			res++;
		}
		/* dispatch order: higher level first, then lower rank */
		for (int i = 1; i < res; i++) {
			typeof(q[0]) k = q[i];
			int j = i;
			for (; j > 0 && (q[j-1].level < k.level ||
					(q[j-1].level == k.level && q[j-1].rank > k.rank)); j--)
				q[j] = q[j-1];
			q[j] = k;
		}
		for (int i = 0; i < res && i < max; i++) {
			if (vectors)
				vectors[i] = q[i].vec;
			if (levels)
				levels[i] = q[i].level;
		}
	}
	host_unlock(h);
	return res;
}

int
avr_host_set_cpu(
		avr_host_t * h,
		const avr_host_cpu_t * in)
{
	int res = 0;
	host_lock(h);
	if (h->mode == AVR_HOST_RUNNING)
		res = host_error(h, AVR_HOST_ESTATE, "pause first");
	else if ((in->pc & 1) || in->pc > h->avr->flashend)
		res = host_error(h, AVR_HOST_ERANGE, "bad pc %#x", in->pc);
	else {
		avr_t * avr = h->avr;
		avr->pc = in->pc;
		memcpy(avr->data, in->r, 32);
		_avr_sp_set(avr, in->sp);
		avr->data[avr->arch.sreg_addr] = in->sreg;
		SET_SREG_FROM(avr, in->sreg);
		h->resume_pc_valid = 0;		/* new PC: its breakpoint must hit */
	}
	host_unlock(h);
	return res;
}

/* ---------------------------------------------------------------------
 * Memory
 * ------------------------------------------------------------------- */

static uint32_t
host_data_size(
		avr_t * avr)
{
	uint32_t size = avr->ramend + 1;
	if (avr->arch.flashmap_start) {
		uint32_t top = avr->arch.flashmap_start + avr->flashend + 1;
		if (top > 0x10000)
			top = 0x10000;
		if (top > size)
			size = top;
	}
	return size;
}

/* Whether 'addr' is backed by something in the data space; the core's
 * accessors would silently wrap the reserved gap into SRAM. */
static int
host_data_mapped(
		avr_t * avr,
		uint16_t addr)
{
	return addr <= avr->ramend ||
		   (avr->arch.flashmap_start && addr >= avr->arch.flashmap_start);
}

/* Raw data-space access: the firmware's view of the address (low-I/O
 * redirect, split SREG, mapped flash) but no IO callbacks. */
static int
host_data_raw(
		avr_t * avr,
		uint16_t addr,
		uint8_t * v,
		int write)
{
	if (addr < AVR_LOWIO_REDIRECT_SIZE && avr->lowio_redirect[addr])
		addr = avr->lowio_redirect[addr];
	if (addr == avr->arch.sreg_addr) {
		if (write) {
			avr->data[addr] = *v;
			SET_SREG_FROM(avr, *v);
		} else
			READ_SREG_INTO(avr, *v);
		return 0;
	}
	if (avr->arch.flashmap_start && addr >= avr->arch.flashmap_start) {
		if (write)
			return AVR_HOST_ERANGE;		/* mapped flash is read-only */
		*v = avr->flash[(addr - avr->arch.flashmap_start) %
						(avr->flashend + 1)];
		return 0;
	}
	if (addr > avr->ramend)
		return AVR_HOST_ERANGE;
	if (write)
		avr->data[addr] = *v;
	else
		*v = avr->data[addr];
	return 0;
}

/* Byte array backing a non-DATA space, or NULL if not available. */
static uint8_t *
host_space(
		avr_t * avr,
		avr_host_space_t space,
		uint32_t * size)
{
	switch (space) {
		case AVR_HOST_MEM_FLASH:
			*size = avr->flashend + 1;
			return avr->flash;
		case AVR_HOST_MEM_EEPROM:
			if ((avr->arch.flags & AVR_ARCH_F_MODERN) && avr->arch.persist_end) {
				/* Modern EEPROM lives in the data space (NVMCTRL). */
				*size = avr->arch.persist_end - avr->arch.persist_start + 1;
				return avr->data + avr->arch.persist_start;
			} else {
				avr_eeprom_desc_t ee = { .ee = NULL, .offset = 0,
										 .size = avr->e2end + 1 };
				if (!avr->e2end ||
						avr_ioctl(avr, AVR_IOCTL_EEPROM_GET, &ee) != 0)
					return NULL;
				*size = avr->e2end + 1;
				return ee.ee;
			}
		case AVR_HOST_MEM_FUSES:
			*size = sizeof(avr->fuse);
			return avr->fuse;
		case AVR_HOST_MEM_SIGNATURE:
			*size = sizeof(avr->signature);
			return avr->signature;
		default:
			return NULL;
	}
}

int
avr_host_mem_size(
		avr_host_t * h,
		avr_host_space_t space,
		uint32_t * out_size)
{
	int res = 0;
	host_lock(h);
	if (space == AVR_HOST_MEM_DATA)
		*out_size = host_data_size(h->avr);
	else if (!host_space(h->avr, space, out_size))
		res = host_error(h, AVR_HOST_ENOTSUP, "memory space %d unavailable",
						 space);
	host_unlock(h);
	return res;
}

static int
host_mem_access(
		avr_host_t * h,
		avr_host_space_t space,
		uint32_t addr,
		uint8_t * buf,
		uint32_t len,
		int side_effects,
		int write)
{
	int res = 0;
	uint32_t size;

	if (!buf && len)
		return host_error(h, AVR_HOST_EINVAL, "NULL buffer");
	host_lock(h);
	avr_t * avr = h->avr;
	if (space == AVR_HOST_MEM_DATA) {
		size = host_data_size(avr);
		if ((uint64_t)addr + len > size) {
			res = host_error(h, AVR_HOST_ERANGE, "data %#x+%u out of range",
							 addr, len);
			goto out;
		}
		for (uint32_t i = 0; i < len && !res; i++) {
			uint16_t a = addr + i;
			if (side_effects && !host_data_mapped(avr, a))
				res = host_error(h, AVR_HOST_ERANGE,
								 "data %#x is not mapped", a);
			else if (side_effects && write)
				avr_core_data_write(avr, a, buf[i]);
			else if (side_effects)
				buf[i] = avr_core_data_read(avr, a);
			else if (host_data_raw(avr, a, &buf[i], write))
				res = host_error(h, AVR_HOST_ERANGE,
								 "data %#x not %s", a,
								 write ? "writable" : "readable");
		}
	} else {
		uint8_t * mem = host_space(avr, space, &size);
		if (!mem)
			res = host_error(h, AVR_HOST_ENOTSUP,
							 "memory space %d unavailable", space);
		else if ((uint64_t)addr + len > size)
			res = host_error(h, AVR_HOST_ERANGE, "%#x+%u out of range",
							 addr, len);
		else if (write)
			memcpy(mem + addr, buf, len);
		else
			memcpy(buf, mem + addr, len);
	}
out:
	host_unlock(h);
	return res;
}

int
avr_host_mem_read(
		avr_host_t * h,
		avr_host_space_t space,
		uint32_t addr,
		uint8_t * buf,
		uint32_t len,
		int side_effects)
{
	return host_mem_access(h, space, addr, buf, len, side_effects, 0);
}

int
avr_host_mem_write(
		avr_host_t * h,
		avr_host_space_t space,
		uint32_t addr,
		const uint8_t * buf,
		uint32_t len,
		int side_effects)
{
	return host_mem_access(h, space, addr, (uint8_t *)buf, len,
						   side_effects, 1);
}

/* ---------------------------------------------------------------------
 * Not implemented yet (see sim_host.h)
 * ------------------------------------------------------------------- */

int
avr_host_reg_count(
		avr_host_t * h)
{
	return host_error(h, AVR_HOST_ENOTSUP, "register table not available");
}

int
avr_host_reg_info(
		avr_host_t * h,
		int index,
		avr_host_reg_info_t * out)
{
	return host_error(h, AVR_HOST_ENOTSUP, "register table not available");
}

int
avr_host_reg_lookup(
		avr_host_t * h,
		const char * name)
{
	return host_error(h, AVR_HOST_ENOTSUP, "register table not available");
}

int
avr_host_symbol_lookup(
		avr_host_t * h,
		const char * name,
		uint32_t * out_addr,
		uint32_t * out_size)
{
	return host_error(h, AVR_HOST_ENOTSUP, "symbols not implemented");
}

const char *
avr_host_symbol_at(
		avr_host_t * h,
		uint32_t flash_addr,
		uint32_t * out_offset)
{
	return NULL;
}

/* Runs on the sim thread, lock held, inside the instruction that accesses
 * data space. Only records the hit; host_exec() pauses once the instruction
 * has completed (so PC is the instruction after the access). */
static void
host_cpu_watch(
		avr_t * avr,
		uint16_t addr,
		uint8_t v,
		int write,
		void * param)
{
	avr_host_t * h = param;

	if (!h->in_run || h->watch_hit)
		return;
	for (int i = 0; i < h->nwatches; i++) {
		uint32_t f = h->watch[i].flags;
		if (addr < h->watch[i].addr ||
			addr >= (uint32_t)h->watch[i].addr + h->watch[i].len)
			continue;
		if (!(f & (write ? AVR_HOST_WATCH_WRITE : AVR_HOST_WATCH_READ)))
			continue;
		if (write && (f & AVR_HOST_WATCH_CHANGE) && avr->data[addr] == v)
			continue;
		h->watch_hit = h->watch[i].id;
		h->watch_addr = addr;
		return;
	}
}

int
avr_host_watch_add(
		avr_host_t * h,
		uint16_t addr,
		uint16_t len,
		uint32_t flags)
{
	int res;
	uint8_t * prev = NULL;
	host_lock(h);
	if (!len || (uint32_t)addr + len > (uint32_t)h->avr->ramend + 1)
		res = host_error(h, AVR_HOST_ERANGE, "bad watch range %#x+%u", addr, len);
	else if (!(flags & (AVR_HOST_WATCH_READ | AVR_HOST_WATCH_WRITE | AVR_HOST_WATCH_PERIPH)))
		res = host_error(h, AVR_HOST_EINVAL, "watch needs READ, WRITE and/or PERIPH");
	else if (h->nwatches == HOST_MAX_WATCHES)
		res = host_error(h, AVR_HOST_ENOMEM, "too many watchpoints");
	else if ((flags & AVR_HOST_WATCH_PERIPH) && !(prev = malloc(len)))
		res = host_error(h, AVR_HOST_ENOMEM, "out of memory");
	else {
		res = h->next_id++;
		h->watch[h->nwatches].id = res;
		h->watch[h->nwatches].addr = addr;
		h->watch[h->nwatches].len = len;
		h->watch[h->nwatches].prev = prev;
		h->watch[h->nwatches++].flags = flags;
		if (prev) {
			memcpy(prev, h->avr->data + addr, len);
			h->nperiph++;
		}
		h->avr->cpu_watch = host_cpu_watch;
		h->avr->cpu_watch_param = h;
	}
	host_unlock(h);
	return res;
}

int
avr_host_watch_remove(
		avr_host_t * h,
		int id)
{
	int res = host_error(h, AVR_HOST_ENOENT, "no watchpoint %d", id);
	host_lock(h);
	for (int i = 0; i < h->nwatches; i++)
		if (h->watch[i].id == id) {
			if (h->watch[i].prev) {
				free(h->watch[i].prev);
				h->nperiph--;
			}
			h->watch[i] = h->watch[--h->nwatches];
			res = 0;
			break;
		}
	if (!h->nwatches) {
		h->avr->cpu_watch = NULL;
		h->watch_hit = 0;
	}
	host_unlock(h);
	return res;
}

int
avr_host_pin_set_mv(
		avr_host_t * h,
		int pin,
		uint32_t millivolts)
{
	return host_error(h, AVR_HOST_ENOTSUP, "analog input not implemented");
}

int
avr_host_poll_text(
		avr_host_t * h,
		char * buf,
		size_t size)
{
	if (buf && size)
		buf[0] = 0;
	return 0;
}

int
avr_host_vcd_add_trace(
		avr_host_t * h,
		const char * spec)
{
	return host_error(h, AVR_HOST_ENOTSUP, "VCD not implemented");
}

int
avr_host_vcd_start(
		avr_host_t * h,
		const char * path,
		uint32_t period_us)
{
	return host_error(h, AVR_HOST_ENOTSUP, "VCD not implemented");
}

int
avr_host_vcd_stop(
		avr_host_t * h)
{
	return host_error(h, AVR_HOST_ENOTSUP, "VCD not implemented");
}

/* ---------------------------------------------------------------------
 * Breakpoints
 * ------------------------------------------------------------------- */

int
avr_host_break_add(
		avr_host_t * h,
		uint32_t pc)
{
	int res;
	host_lock(h);
	if ((pc & 1) || pc > h->avr->flashend)
		res = host_error(h, AVR_HOST_ERANGE, "bad breakpoint pc %#x", pc);
	else if (h->nbreaks == HOST_MAX_BREAKS)
		res = host_error(h, AVR_HOST_ENOMEM, "too many breakpoints");
	else {
		res = h->next_id++;
		h->break_pc[h->nbreaks] = pc;
		h->break_id[h->nbreaks++] = res;
	}
	host_unlock(h);
	return res;
}

int
avr_host_break_remove(
		avr_host_t * h,
		int id)
{
	int res = host_error(h, AVR_HOST_ENOENT, "no breakpoint %d", id);
	host_lock(h);
	for (int i = 0; i < h->nbreaks; i++)
		if (h->break_id[i] == id) {
			h->nbreaks--;
			h->break_pc[i] = h->break_pc[h->nbreaks];
			h->break_id[i] = h->break_id[h->nbreaks];
			res = 0;
			break;
		}
	host_unlock(h);
	return res;
}

/* ---------------------------------------------------------------------
 * Attached parts
 * ------------------------------------------------------------------- */

static int
host_pin_to_port(avr_host_t * h, int pin, char * port, uint8_t * bit)
{
	if (pin < 0)
		return 0;			/* not wired */
	if (pin >= HOST_MAX_PINS || !(h->ports & (1 << (pin / 8))))
		return -1;
	*port = 'A' + pin / 8;
	*bit = pin % 8;
	return 1;
}

int
avr_host_oled_attach(
		avr_host_t * h,
		const avr_host_oled_cfg_t * cfg)
{
	if (!cfg || (cfg->bus != AVR_HOST_OLED_TWI && cfg->bus != AVR_HOST_OLED_SPI) ||
			cfg->instance > 9)
		return host_error(h, AVR_HOST_EINVAL, "bad OLED configuration");
	if (cfg->bus == AVR_HOST_OLED_TWI && cfg->i2c_addr != 0x3C && cfg->i2c_addr != 0x3D)
		return host_error(h, AVR_HOST_EINVAL, "SSD1306 I2C address is 0x3C or 0x3D");
	if (cfg->bus == AVR_HOST_OLED_SPI && cfg->dc_pin < 0)
		return host_error(h, AVR_HOST_EINVAL, "SPI OLED needs a D/C pin");
	int res;
	host_lock(h);
	char cs = 0, dc = 0, rs = 0;
	uint8_t csb = 0, dcb = 0, rsb = 0;
	int slot = 0;
	while (slot < HOST_MAX_OLEDS && h->oled[slot])
		slot++;
	if (slot == HOST_MAX_OLEDS) {
		res = host_error(h, AVR_HOST_ERANGE, "too many OLEDs");
	} else if (host_pin_to_port(h, cfg->cs_pin, &cs, &csb) < 0 ||
			host_pin_to_port(h, cfg->dc_pin, &dc, &dcb) < 0 ||
			host_pin_to_port(h, cfg->res_pin, &rs, &rsb) < 0) {
		res = host_error(h, AVR_HOST_ENOENT, "OLED pin not on this device");
	} else {
		char bus = '0' + cfg->instance;
		avr_ssd1306_t * p = cfg->bus == AVR_HOST_OLED_TWI ?
				avr_ssd1306_new_twi(h->avr, bus, cfg->i2c_addr, rs, rsb) :
				avr_ssd1306_new_spi(h->avr, bus, cs, csb, dc, dcb, rs, rsb);
		if (!p) {
			res = host_error(h, AVR_HOST_ENOENT, "no %s%d on this device",
					cfg->bus == AVR_HOST_OLED_TWI ? "TWI" : "SPI", cfg->instance);
		} else {
			h->oled[slot] = p;
			res = slot;
		}
	}
	host_unlock(h);
	return res;
}

int
avr_host_oled_read(
		avr_host_t * h,
		int id,
		avr_host_oled_t * out)
{
	int res = 0;
	if (!out)
		return host_error(h, AVR_HOST_EINVAL, "no output buffer");
	host_lock(h);
	if (id < 0 || id >= HOST_MAX_OLEDS || !h->oled[id]) {
		res = host_error(h, AVR_HOST_ENOENT, "no OLED %d", id);
	} else {
		avr_ssd1306_state_t s;
		avr_ssd1306_snapshot(h->oled[id], &s);
		out->seq = s.seq;
		out->on = s.on;
		out->all_on = s.all_on;
		out->inverse = s.inverse;
		out->remap = s.remap;
		out->com_flip = s.com_flip;
		out->contrast = s.contrast;
		out->start_line = s.start_line;
		out->addr_mode = s.addr_mode;
		memcpy(out->vram, s.vram, sizeof(out->vram));
	}
	host_unlock(h);
	return res;
}

int
avr_host_oled_detach(
		avr_host_t * h,
		int id)
{
	int res = 0;
	host_lock(h);
	if (id < 0 || id >= HOST_MAX_OLEDS || !h->oled[id]) {
		res = host_error(h, AVR_HOST_ENOENT, "no OLED %d", id);
	} else {
		avr_ssd1306_free(h->oled[id]);
		h->oled[id] = NULL;
	}
	host_unlock(h);
	return res;
}

/* ---------------------------------------------------------------------
 * Pins
 * ------------------------------------------------------------------- */

int
avr_host_pin_count(
		avr_host_t * h)
{
	int n = 0;
	for (int pi = 0; pi < HOST_MAX_PORTS; pi++)
		if (h->ports & (1 << pi))
			n = (pi + 1) * 8;
	return n;
}

int
avr_host_pin_lookup(
		avr_host_t * h,
		const char * name)
{
	if (name && toupper((unsigned char)name[0]) == 'P' && name[1] &&
			name[2] >= '0' && name[2] <= '7' && !name[3]) {
		int pi = toupper((unsigned char)name[1]) - 'A';
		if (pi >= 0 && pi < HOST_MAX_PORTS && (h->ports & (1 << pi)))
			return pi * 8 + (name[2] - '0');
	}
	return host_error(h, AVR_HOST_ENOENT, "no pin '%s'", name ? name : "");
}

const char *
avr_host_pin_name(
		avr_host_t * h,
		int pin)
{
	if (pin < 0 || pin >= HOST_MAX_PINS || !(h->ports & (1 << (pin / 8))))
		return NULL;
	return h->pin_name[pin];
}

int
avr_host_pin_get(
		avr_host_t * h,
		int pin,
		avr_host_pin_t * out)
{
	char port = 0;
	int bit = 0, res;
	if ((res = host_parse_pin(h, pin, &port, &bit)))
		return res;
	host_lock(h);
	avr_t * avr = h->avr;
	avr_ioport_state_t st;
	avr_ioctl(avr, AVR_IOCTL_IOPORT_GETSTATE(port), &st);
	uint8_t m = 1 << bit;
	uint8_t inv = 0, pull;
	avr_port_modern_t * mp = host_modern_port(avr, port);
	if (mp) {
		uint8_t ctrl = avr->data[mp->r_pinctrl + bit];
		inv = (ctrl & PINCTRL_INVEN) ? m : 0;
		pull = (ctrl & PINCTRL_PULLUPEN) ? 1 : 0;
	} else
		pull = !(st.ddr & m) && (st.port & m);		/* classic: PORTx=1 on input */
	/* Same derivation as the modern IN read: outputs read back OUT. */
	uint8_t in = (((st.pin & ~st.ddr) | (st.port & st.ddr)) ^ inv) & m;
	out->dir = !!(st.ddr & m);
	out->out = !!(st.port & m);
	out->in = !!in;
	out->pullup = pull && !out->dir;
	out->driven = !!(h->ext_mask[pin / 8] & m);
	out->floating = !out->dir && !out->pullup && !out->driven;
	host_unlock(h);
	return 0;
}

static int
host_pin_set_drive(
		avr_host_t * h,
		int pin,
		int drive,
		int level)
{
	char port = 0;
	int bit = 0, res;
	if ((res = host_parse_pin(h, pin, &port, &bit)))
		return res;
	host_lock(h);
	int pi = pin / 8;
	uint8_t m = 1 << bit;
	uint8_t old_mask = h->ext_mask[pi], old_value = h->ext_value[pi];
	if (drive) {
		h->ext_mask[pi] |= m;
		h->ext_value[pi] = level ? (h->ext_value[pi] | m) :
								   (h->ext_value[pi] & ~m);
	} else {
		h->ext_mask[pi] &= ~m;
		h->ext_value[pi] &= ~m;
	}
	if ((res = host_apply_external(h, pi, m))) {
		h->ext_mask[pi] = old_mask;
		h->ext_value[pi] = old_value;
	}
	host_unlock(h);
	return res;
}

int
avr_host_pin_drive(
		avr_host_t * h,
		int pin,
		int level)
{
	return host_pin_set_drive(h, pin, 1, level);
}

int
avr_host_pin_release(
		avr_host_t * h,
		int pin)
{
	return host_pin_set_drive(h, pin, 0, 0);
}

/* ---------------------------------------------------------------------
 * Serial
 * ------------------------------------------------------------------- */

int
avr_host_uart_send(
		avr_host_t * h,
		int index,
		const uint8_t * data,
		uint32_t len)
{
	int res = 0;
	if (index < 0 || index >= HOST_MAX_UARTS || !h->uart[index].io)
		return host_error(h, AVR_HOST_ENOENT, "no USART%d", index);
	if (!data && len)
		return host_error(h, AVR_HOST_EINVAL, "NULL buffer");
	host_lock(h);
	if (h->uart[index].len + len > h->uart[index].size) {
		uint32_t size = (h->uart[index].len + len) * 2;
		uint8_t * b = realloc(h->uart[index].buf, size);
		if (!b) {
			res = host_error(h, AVR_HOST_ENOMEM, "out of memory");
			goto out;
		}
		h->uart[index].buf = b;
		h->uart[index].size = size;
	}
	memcpy(h->uart[index].buf + h->uart[index].len, data, len);
	h->uart[index].len += len;
	host_feed_uarts(h);
out:
	host_unlock(h);
	return res;
}

/* ---------------------------------------------------------------------
 * Subscriptions / events
 * ------------------------------------------------------------------- */

static void
host_sub_notify(
		struct avr_irq_t * irq,
		uint32_t value,
		void * param)
{
	host_sub_t * s = param;
	avr_host_t * h = s->h;
	uint64_t cycle = h->avr->cycle;

	switch (s->kind) {
		case SUB_PIN:
			value &= 1;
			if (s->coalesce && cycle - s->last_emit < s->coalesce) {
				s->pending = 1;
				s->pending_value = value;
				s->pending_cycle = cycle;
				break;
			}
			s->pending = 0;
			s->last_emit = cycle;
			host_push(h, AVR_HOST_EV_PIN, s->id, cycle, s->arg, value, 0);
			break;
		case SUB_UART:
			host_push(h, AVR_HOST_EV_UART_TX, s->id, cycle, s->arg,
					  value & 0xff, 0);
			break;
		case SUB_IRQ:
			host_push(h, AVR_HOST_EV_IRQ, s->id, cycle, s->arg, value, 0);
			break;
		case SUB_INT: {
			/* The table IRQ carries the running vector on dispatch and the
			 * vector being returned to (or 0) on RETI. Dispatch notifies
			 * before the vector is pushed on running[]; RETI after the pop,
			 * so a notify naming the top of the stack is a return. */
			avr_int_table_t * t = &h->avr->interrupts;
			if (!value || (t->running_ptr && t->running[t->running_ptr - 1]->vector == value))
				break;
			uint32_t level = 0;
			if (t->cpuint_status & AVR_CPUINT_NMIEX)
				level = 2;
			else if (t->cpuint_status & AVR_CPUINT_LVL1EX)
				level = 1;
			host_push(h, AVR_HOST_EV_INTERRUPT, s->id, cycle, value, level, 0);
			break;
		}
	}
}

/* Called with the lock held. */
static int
host_subscribe(
		avr_host_t * h,
		int kind,
		avr_irq_t * irq,
		uint32_t arg,
		uint32_t coalesce)
{
	host_sub_t * s = calloc(1, sizeof(*s));
	if (!s)
		return host_error(h, AVR_HOST_ENOMEM, "out of memory");
	s->h = h;
	s->id = h->next_id++;
	s->kind = kind;
	s->irq = irq;
	s->arg = arg;
	s->coalesce = coalesce;
	s->last_emit = h->avr->cycle - coalesce;	/* first change emits at once */
	s->next = h->subs;
	h->subs = s;
	avr_irq_register_notify(irq, host_sub_notify, s);
	return s->id;
}

int
avr_host_subscribe_pin(
		avr_host_t * h,
		int pin,
		uint32_t coalesce_cycles)
{
	char port = 0;
	int bit = 0, res;
	if ((res = host_parse_pin(h, pin, &port, &bit)))
		return res;
	host_lock(h);
	avr_irq_t * irq = avr_io_getirq(h->avr, AVR_IOCTL_IOPORT_GETIRQ(port), bit);
	res = irq ? host_subscribe(h, SUB_PIN, irq, pin, coalesce_cycles) :
				host_error(h, AVR_HOST_ENOENT, "no IRQ for pin %d", pin);
	host_unlock(h);
	return res;
}

int
avr_host_subscribe_uart(
		avr_host_t * h,
		int index)
{
	int res;
	if (index < 0 || index >= HOST_MAX_UARTS || !h->uart[index].io)
		return host_error(h, AVR_HOST_ENOENT, "no USART%d", index);
	host_lock(h);
	res = host_subscribe(h, SUB_UART,
			h->uart[index].io->irq + UART_IRQ_OUTPUT, index, 0);
	host_unlock(h);
	return res;
}

int
avr_host_subscribe_interrupts(
		avr_host_t * h)
{
	int res;
	host_lock(h);
	res = host_subscribe(h, SUB_INT, h->avr->interrupts.irq + AVR_INT_IRQ_RUNNING, 0, 0);
	host_unlock(h);
	return res;
}

int
avr_host_subscribe_irq(
		avr_host_t * h,
		uint32_t ioctl,
		int irq_index)
{
	int res;
	host_lock(h);
	avr_irq_t * irq = avr_io_getirq(h->avr, ioctl, irq_index);
	res = irq ? host_subscribe(h, SUB_IRQ, irq, irq_index, 0) :
				host_error(h, AVR_HOST_ENOENT, "no IRQ %#x/%d", ioctl,
						   irq_index);
	host_unlock(h);
	return res;
}

int
avr_host_unsubscribe(
		avr_host_t * h,
		int sub)
{
	int res = host_error(h, AVR_HOST_ENOENT, "no subscription %d", sub);
	host_lock(h);
	for (host_sub_t ** p = &h->subs; *p; p = &(*p)->next) {
		host_sub_t * s = *p;
		if (s->id != sub)
			continue;
		*p = s->next;
		avr_irq_unregister_notify(s->irq, host_sub_notify, s);
		free(s);
		res = 0;
		break;
	}
	host_unlock(h);
	return res;
}

int
avr_host_poll_events(
		avr_host_t * h,
		avr_host_event_t * out,
		int max)
{
	int n = 0;
	pthread_mutex_lock(&h->ev_lock);
	while (n < max && h->ev_count) {
		out[n++] = h->evq[h->ev_head];
		h->ev_head = (h->ev_head + 1) % HOST_EVQ_SIZE;
		h->ev_count--;
	}
	if (h->ev_dropped && n < max) {
		memset(&out[n], 0, sizeof(out[n]));
		out[n].type = AVR_HOST_EV_OVERFLOW;
		out[n++].a = h->ev_dropped;
		h->ev_dropped = 0;
	}
#ifndef _WIN32
	if (!h->ev_count && !h->ev_dropped && h->ev_pipe[0] >= 0) {
		uint8_t drain[64];
		while (read(h->ev_pipe[0], drain, sizeof(drain)) > 0)
			;
	}
#endif
	pthread_mutex_unlock(&h->ev_lock);
	return n;
}

int
avr_host_event_fd(
		avr_host_t * h)
{
	return h->ev_pipe[0];
}

/* ---------------------------------------------------------------------
 * Native escape hatch
 * ------------------------------------------------------------------- */

struct avr_t *
avr_host_raw_avr(
		avr_host_t * h)
{
	return h->avr;
}

void
avr_host_lock(
		avr_host_t * h)
{
	host_lock(h);
}

void
avr_host_unlock(
		avr_host_t * h)
{
	host_unlock(h);
}
