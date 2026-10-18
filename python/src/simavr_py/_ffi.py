"""cffi (ABI mode) declarations for simavr/sim/sim_host.h and the loader for
libsimavr.

The cdef below mirrors sim_host.h by hand; tests/test_bindings.py checks it
against the header (function list) and against the C compiler (struct
layouts), so drift is caught rather than silently mis-marshalled.
"""

import os
import threading
from pathlib import Path

from cffi import FFI

lazy import ctypes.util
lazy import glob

CDEF = """
enum {
    AVR_HOST_OK = 0,
    AVR_HOST_EINVAL = -1,
    AVR_HOST_ENOENT = -2,
    AVR_HOST_ESTATE = -3,
    AVR_HOST_ERANGE = -4,
    AVR_HOST_ENOMEM = -5,
    AVR_HOST_EIO = -6,
    AVR_HOST_ENOTSUP = -7,
};

typedef struct avr_host_t avr_host_t;

typedef struct avr_host_cpu_t {
    uint32_t pc;
    uint16_t sp;
    uint8_t  sreg;
    uint8_t  r[32];
    uint64_t cycle;
} avr_host_cpu_t;

typedef struct avr_host_reg_info_t {
    const char * name;
    const char * peripheral;
    uint16_t     addr;
    uint8_t      size;
    uint8_t      reset;
} avr_host_reg_info_t;

typedef struct avr_host_pin_t {
    uint8_t dir, out, in, pullup, driven, floating;
} avr_host_pin_t;

typedef struct avr_host_event_t {
    uint32_t type;
    int32_t  sub;
    uint64_t cycle;
    uint32_t a, b, c;
} avr_host_event_t;

int avr_host_api_version(void);
int avr_host_core_count(void);
const char * avr_host_core_name(int index);

avr_host_t * avr_host_new(const char * mmcu, uint32_t freq_hz);
avr_host_t * avr_host_new_from_elf(const char * path,
        const char * fallback_mmcu, uint32_t fallback_freq_hz);
void avr_host_free(avr_host_t * h);
const char * avr_host_last_error(avr_host_t * h);
int avr_host_last_error_code(avr_host_t * h);
const char * avr_host_mmcu(avr_host_t * h);
uint32_t avr_host_frequency(avr_host_t * h);

int avr_host_load_elf(avr_host_t * h, const char * path);
int avr_host_load_hex(avr_host_t * h, const char * path);
int avr_host_load_flash(avr_host_t * h, const uint8_t * data, uint32_t size,
        uint32_t addr);

int avr_host_start(avr_host_t * h);
int avr_host_resume(avr_host_t * h);
int avr_host_pause(avr_host_t * h);
int avr_host_step(avr_host_t * h, uint32_t count);
int avr_host_run_cycles(avr_host_t * h, uint64_t cycles);
int avr_host_reset(avr_host_t * h);
int avr_host_get_state(avr_host_t * h);
int avr_host_wait_stopped(avr_host_t * h, int timeout_ms);
int avr_host_set_speed(avr_host_t * h, double ratio);
int avr_host_set_batch(avr_host_t * h, uint32_t cycles);
uint64_t avr_host_cycle(avr_host_t * h);
uint64_t avr_host_time_ns(avr_host_t * h);

int avr_host_get_cpu(avr_host_t * h, avr_host_cpu_t * out);
int avr_host_set_cpu(avr_host_t * h, const avr_host_cpu_t * in);

int avr_host_mem_size(avr_host_t * h, int space, uint32_t * out_size);
int avr_host_mem_read(avr_host_t * h, int space, uint32_t addr,
        uint8_t * buf, uint32_t len, int side_effects);
int avr_host_mem_write(avr_host_t * h, int space, uint32_t addr,
        const uint8_t * buf, uint32_t len, int side_effects);

int avr_host_reg_count(avr_host_t * h);
int avr_host_reg_info(avr_host_t * h, int index, avr_host_reg_info_t * out);
int avr_host_reg_lookup(avr_host_t * h, const char * name);

int avr_host_symbol_lookup(avr_host_t * h, const char * name,
        uint32_t * out_addr, uint32_t * out_size);
const char * avr_host_symbol_at(avr_host_t * h, uint32_t flash_addr,
        uint32_t * out_offset);

int avr_host_break_add(avr_host_t * h, uint32_t pc);
int avr_host_break_remove(avr_host_t * h, int id);
int avr_host_watch_add(avr_host_t * h, uint16_t addr, uint16_t len,
        uint32_t flags);
int avr_host_watch_remove(avr_host_t * h, int id);

int avr_host_pin_count(avr_host_t * h);
int avr_host_pin_lookup(avr_host_t * h, const char * name);
const char * avr_host_pin_name(avr_host_t * h, int pin);
int avr_host_pin_get(avr_host_t * h, int pin, avr_host_pin_t * out);
int avr_host_pin_drive(avr_host_t * h, int pin, int level);
int avr_host_pin_release(avr_host_t * h, int pin);
int avr_host_pin_set_mv(avr_host_t * h, int pin, uint32_t millivolts);

int avr_host_uart_send(avr_host_t * h, int index, const uint8_t * data,
        uint32_t len);

int avr_host_subscribe_pin(avr_host_t * h, int pin, uint32_t coalesce_cycles);
int avr_host_subscribe_uart(avr_host_t * h, int index);
int avr_host_subscribe_interrupts(avr_host_t * h);
int avr_host_subscribe_irq(avr_host_t * h, uint32_t ioctl, int irq_index);
int avr_host_unsubscribe(avr_host_t * h, int sub);
int avr_host_poll_events(avr_host_t * h, avr_host_event_t * out, int max);
int avr_host_event_fd(avr_host_t * h);
int avr_host_poll_text(avr_host_t * h, char * buf, size_t size);

int avr_host_vcd_add_trace(avr_host_t * h, const char * spec);
int avr_host_vcd_start(avr_host_t * h, const char * path, uint32_t period_us);
int avr_host_vcd_stop(avr_host_t * h);

struct avr_t;
struct avr_t * avr_host_raw_avr(avr_host_t * h);
void avr_host_lock(avr_host_t * h);
void avr_host_unlock(avr_host_t * h);
"""

ffi = FFI()
ffi.cdef(CDEF)

# Repo root when running from a source checkout (python/src/simavr_py/..).
_REPO = Path(__file__).resolve().parents[3]

_lib = None
_lib_lock = threading.Lock()


def _candidates():
    if env := os.environ.get("SIMAVR_LIB"):
        yield env
        return
    yield from sorted(glob.glob(str(_REPO / "simavr" / "obj-*" / "libsimavr.so")))
    if found := ctypes.util.find_library("simavr"):
        yield found
    yield "libsimavr.so.1"


def lib():
    """The loaded libsimavr (loaded once, on first use).

    Set SIMAVR_LIB to a library path to override the search, which otherwise
    tries the checkout's simavr/obj-*/libsimavr.so, then the system library.
    """
    global _lib
    if _lib is not None:
        return _lib
    with _lib_lock:
        if _lib is None:
            errors = []
            for path in _candidates():
                try:
                    _lib = ffi.dlopen(path)
                    break
                except OSError as e:
                    errors.append(f"{path}: {e}")
            else:
                raise OSError("cannot load libsimavr; build it with "
                              "`make -C simavr` or set SIMAVR_LIB\n  "
                              + "\n  ".join(errors))
    return _lib
