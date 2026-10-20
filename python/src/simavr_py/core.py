"""Pythonic wrapper over the simavr host API (simavr/sim/sim_host.h).

Thread model: every call goes straight into the C library with the GIL
released (cffi does that for us), and the C side serialises access with its
own instance lock, so a Simulator may be used from several Python threads
at once, on GIL and free-threaded interpreters alike. The one rule: don't
call close() while another thread is still using the instance.
"""

import enum
import os
import weakref
from collections.abc import Iterator
from dataclasses import dataclass

from ._ffi import ffi, lib

lazy import select

type PinRef = int | str
type Buffer = bytes | bytearray | memoryview


# ----------------------------------------------------------------------------
# Constants (values mirror sim_host.h)


class State(enum.IntEnum):
    PAUSED = 0
    RUNNING = 1
    SLEEPING = 2
    DONE = 3
    CRASHED = 4


class StopReason(enum.IntEnum):
    NONE = 0
    USER = 1
    STEP = 2
    BREAKPOINT = 3
    WATCHPOINT = 4
    DONE = 5
    CRASH = 6


class WatchFlag(enum.IntFlag):
    READ = 1
    WRITE = 2
    CHANGE = 4          # with WRITE: only when the value differs
    PERIPH = 8          # any change, whoever wrote it (peripheral models included)


class Space(enum.IntEnum):
    DATA = 0
    FLASH = 1
    EEPROM = 2
    FUSES = 3
    SIGNATURE = 4


class _Ev(enum.IntEnum):
    STATE = 1
    PIN = 2
    UART_TX = 3
    WATCH = 4
    IRQ = 5
    INTERRUPT = 6
    LOG = 7
    OVERFLOW = 8


# ----------------------------------------------------------------------------
# Errors


class SimavrError(Exception):
    """A host API call failed; `code` is the AVR_HOST_E* value."""

    def __init__(self, code: int, message: str):
        super().__init__(message or f"simavr error {code}")
        self.code = code


class InvalidArgument(SimavrError, ValueError): pass
class NotFound(SimavrError, LookupError): pass
class StateError(SimavrError): pass
class OutOfRange(SimavrError, IndexError): pass
class LoadError(SimavrError, OSError): pass
class NotSupported(SimavrError, NotImplementedError): pass


_ERRORS = frozendict({
    -1: InvalidArgument,
    -2: NotFound,
    -3: StateError,
    -4: OutOfRange,
    -5: MemoryError,
    -6: LoadError,
    -7: NotSupported,
})


def _raise(code: int, message: str):
    cls = _ERRORS.get(code, SimavrError)
    if cls is MemoryError:
        raise MemoryError(message)
    raise cls(code, message)


def _last_error(L) -> tuple[int, str]:
    """The calling thread's last failure (the C side keeps it per thread)."""
    return (L.avr_host_last_error_code(ffi.NULL),
            ffi.string(L.avr_host_last_error(ffi.NULL)).decode())


_U32 = (1 << 32) - 1
_U64 = (1 << 64) - 1
_INT_MAX = (1 << 31) - 1


def _uint(name: str, value: int, limit: int = _U32) -> int:
    """Validate an unsigned C argument so bad values raise InvalidArgument,
    not a bare cffi OverflowError."""
    if not isinstance(value, int) or isinstance(value, bool) or \
            not 0 <= value <= limit:
        raise InvalidArgument(-1, f"{name} must be an int in 0..{limit}, "
                                  f"got {value!r}")
    return value


def ioctl_def(tag: str, name: str) -> int:
    """AVR_IOCTL_DEF from a 3-letter tag and instance name, e.g.
    ioctl_def("tca", "0") == AVR_IOCTL_TCA_GETIRQ('0')."""
    a, b, c = (ord(ch) for ch in tag)
    return (a << 24) | (b << 16) | (c << 8) | ord(name)


# ----------------------------------------------------------------------------
# Value types


@dataclass(frozen=True, slots=True)
class Cpu:
    pc: int
    sp: int
    sreg: int
    r: bytes
    cycle: int = 0


@dataclass(frozen=True, slots=True)
class PinState:
    dir: bool
    out: bool
    level: bool
    pullup: bool
    driven: bool
    floating: bool


@dataclass(frozen=True, slots=True)
class OledState:
    """Snapshot of an attached SSD1306: GDDRAM plus the display settings.
    `seq` changes whenever anything here does. `vram` is 8 pages x 128
    columns, page-major (index page * 128 + column); bit 0 is the top row."""
    seq: int
    on: bool
    all_on: bool
    inverse: bool
    remap: bool
    com_flip: bool
    contrast: int
    start_line: int
    addr_mode: int      # 0 horizontal, 1 vertical, 2 page
    vram: bytes


@dataclass(frozen=True, slots=True)
class Event:
    sub: int
    cycle: int


@dataclass(frozen=True, slots=True)
class StateEvent(Event):
    state: State
    reason: StopReason
    id: int


@dataclass(frozen=True, slots=True)
class PinEvent(Event):
    pin: int
    level: bool


@dataclass(frozen=True, slots=True)
class UartTxEvent(Event):
    index: int
    byte: int


@dataclass(frozen=True, slots=True)
class IrqEvent(Event):
    index: int
    value: int


@dataclass(frozen=True, slots=True)
class InterruptEvent(Event):
    vector: int
    level: int          # CPUINT level: 0 LVL0 (and every classic core), 1 LVL1, 2 NMI


@dataclass(frozen=True, slots=True)
class OverflowEvent(Event):
    dropped: int


@dataclass(frozen=True, slots=True)
class RawEvent(Event):
    type: int
    a: int
    b: int
    c: int


def _event(e) -> Event:
    match _Ev(e.type) if e.type in _Ev else e.type:
        case _Ev.STATE:
            return StateEvent(e.sub, e.cycle, State(e.a), StopReason(e.b), e.c)
        case _Ev.PIN:
            return PinEvent(e.sub, e.cycle, e.a, bool(e.b))
        case _Ev.UART_TX:
            return UartTxEvent(e.sub, e.cycle, e.a, e.b)
        case _Ev.IRQ:
            return IrqEvent(e.sub, e.cycle, e.a, e.b)
        case _Ev.INTERRUPT:
            return InterruptEvent(e.sub, e.cycle, e.a, e.b)
        case _Ev.OVERFLOW:
            return OverflowEvent(e.sub, e.cycle, e.a)
        case other:
            return RawEvent(e.sub, e.cycle, int(other), e.a, e.b, e.c)


# ----------------------------------------------------------------------------
# Library-level queries


def api_version() -> int:
    return lib().avr_host_api_version()


def cores() -> tuple[str, ...]:
    """Names of the cores compiled into libsimavr."""
    L = lib()
    return tuple(ffi.string(L.avr_host_core_name(i)).decode()
                 for i in range(L.avr_host_core_count()))


# ----------------------------------------------------------------------------
# Simulator


class Simulator:
    """One simulated MCU.

    >>> with Simulator("atmega4809") as sim:
    ...     sim.load_flash(image)
    ...     sim.step(10)
    """

    def __init__(self, mmcu: str, frequency: int = 0, *, _handle=None):
        L = lib()
        h = _handle if _handle is not None else \
            L.avr_host_new(mmcu.encode(), _uint("frequency", frequency))
        if h == ffi.NULL:
            _raise(*_last_error(L))
        self._L = L
        self._h = h
        self._finalizer = weakref.finalize(self, L.avr_host_free, h)
        self._pins = self._pin_table()

    @classmethod
    def from_elf(cls, path: str | os.PathLike, mmcu: str | None = None,
                 frequency: int = 0) -> Simulator:
        """Create from an ELF, taking mmcu/frequency from its .mmcu section
        (mmcu/frequency are fallbacks for an ELF without one)."""
        L = lib()
        h = L.avr_host_new_from_elf(os.fsencode(path),
                                    mmcu.encode() if mmcu else ffi.NULL,
                                    _uint("frequency", frequency))
        if h == ffi.NULL:
            _raise(*_last_error(L))
        return cls("", _handle=h)

    # -- lifecycle -----------------------------------------------------------

    def close(self) -> None:
        """Stop the sim thread (if any) and free the instance. Idempotent."""
        self._finalizer()

    def __enter__(self) -> Simulator:
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    @property
    def closed(self) -> bool:
        return not self._finalizer.alive

    def _handle(self):
        if not self._finalizer.alive:
            raise StateError(-3, "simulator is closed")
        return self._h

    def _check(self, rc: int) -> int:
        if rc < 0:
            _raise(rc, _last_error(self._L)[1])
        return rc

    def __repr__(self) -> str:
        if self.closed:
            return "<Simulator closed>"
        return f"<Simulator {self.mmcu} {self.state.name} cycle={self.cycle}>"

    # -- identity / time -----------------------------------------------------

    @property
    def mmcu(self) -> str:
        return ffi.string(self._L.avr_host_mmcu(self._handle())).decode()

    @property
    def frequency(self) -> int:
        """Current CPU clock in Hz (owned by CLKCTRL on modern cores)."""
        return self._L.avr_host_frequency(self._handle())

    @property
    def cycle(self) -> int:
        return self._L.avr_host_cycle(self._handle())

    @property
    def time_ns(self) -> int:
        return self._L.avr_host_time_ns(self._handle())

    # -- firmware ------------------------------------------------------------

    def load_elf(self, path: str | os.PathLike) -> None:
        self._check(self._L.avr_host_load_elf(self._handle(), os.fsencode(path)))

    def load_hex(self, path: str | os.PathLike) -> None:
        self._check(self._L.avr_host_load_hex(self._handle(), os.fsencode(path)))

    def load_flash(self, image: Buffer, address: int = 0) -> None:
        buf = ffi.from_buffer("uint8_t[]", image)
        self._check(self._L.avr_host_load_flash(self._handle(), buf, len(buf),
                                                _uint("address", address)))

    # -- run control ---------------------------------------------------------

    def start(self) -> None:
        """Spawn the simulation thread (instance stays paused)."""
        self._check(self._L.avr_host_start(self._handle()))

    def resume(self) -> None:
        self._check(self._L.avr_host_resume(self._handle()))

    def pause(self) -> None:
        self._check(self._L.avr_host_pause(self._handle()))

    def step(self, count: int = 1) -> None:
        self._check(self._L.avr_host_step(self._handle(), _uint("count", count)))

    def run_cycles(self, cycles: int) -> None:
        self._check(self._L.avr_host_run_cycles(self._handle(),
                                                _uint("cycles", cycles, _U64)))

    def reset(self) -> None:
        self._check(self._L.avr_host_reset(self._handle()))

    @property
    def state(self) -> State:
        return State(self._L.avr_host_get_state(self._handle()))

    def wait_stopped(self, timeout: float | None = None) -> bool:
        """Wait until not running; False if `timeout` seconds elapsed first."""
        ms = -1 if timeout is None else min(_INT_MAX, max(0, round(timeout * 1000)))
        rc = self._L.avr_host_wait_stopped(self._handle(), ms)
        if rc == -3:
            return False
        self._check(rc)
        return True

    def set_speed(self, ratio: float) -> None:
        """1.0 = real time, 0 = unthrottled (default)."""
        self._check(self._L.avr_host_set_speed(self._handle(), ratio))

    def set_batch(self, cycles: int) -> None:
        self._check(self._L.avr_host_set_batch(self._handle(),
                                               _uint("cycles", cycles)))

    # -- CPU / memory --------------------------------------------------------

    @property
    def cpu(self) -> Cpu:
        c = ffi.new("avr_host_cpu_t *")
        self._check(self._L.avr_host_get_cpu(self._handle(), c))
        return Cpu(c.pc, c.sp, c.sreg, bytes(c.r), c.cycle)

    @cpu.setter
    def cpu(self, cpu: Cpu) -> None:
        if len(cpu.r) != 32:
            raise InvalidArgument(-1, "Cpu.r must be 32 bytes")
        c = ffi.new("avr_host_cpu_t *", {"pc": cpu.pc, "sp": cpu.sp,
                                          "sreg": cpu.sreg, "r": list(cpu.r)})
        self._check(self._L.avr_host_set_cpu(self._handle(), c))

    def pending_interrupts(self) -> list[int]:
        """Vectors raised and enabled but not yet taken, ascending. Only while paused.
        Whether one is taken next still depends on SREG.I and the CPUINT level."""
        buf = ffi.new("uint8_t[256]")
        n = self._check(self._L.avr_host_pending_interrupts(self._handle(), buf, 256))
        return list(buf[0:min(n, 256)])

    def mem_size(self, space: Space) -> int:
        n = ffi.new("uint32_t *")
        self._check(self._L.avr_host_mem_size(self._handle(), space, n))
        return n[0]

    def read(self, space: Space, address: int, length: int, *,
             side_effects: bool = False) -> bytes:
        buf = ffi.new("uint8_t[]", _uint("length", length))
        self._check(self._L.avr_host_mem_read(self._handle(), space,
                                              _uint("address", address),
                                              buf, length, side_effects))
        return bytes(ffi.buffer(buf, length))

    def write(self, space: Space, address: int, data: Buffer, *,
              side_effects: bool = False) -> None:
        buf = ffi.from_buffer("uint8_t[]", data)
        self._check(self._L.avr_host_mem_write(self._handle(), space,
                                               _uint("address", address),
                                               buf, len(buf), side_effects))

    def peek(self, address: int, *, side_effects: bool = False) -> int:
        """One data-space byte."""
        return self.read(Space.DATA, address, 1, side_effects=side_effects)[0]

    def poke(self, address: int, value: int, *, side_effects: bool = True) -> None:
        """Write one data-space byte (with IO side effects by default, like ST)."""
        self.write(Space.DATA, address, bytes((value,)), side_effects=side_effects)

    # -- breakpoints ---------------------------------------------------------

    def add_breakpoint(self, pc: int) -> int:
        return self._check(self._L.avr_host_break_add(self._handle(),
                                                      _uint("pc", pc)))

    def remove_breakpoint(self, bp: int) -> None:
        self._check(self._L.avr_host_break_remove(self._handle(), bp))

    # -- data watchpoints ------------------------------------------------------

    def add_watchpoint(self, addr: int, length: int = 1,
                       flags: WatchFlag = WatchFlag.WRITE) -> int:
        """Pause when the CPU accesses data-space [addr, addr+length). The stop
        is reported (StopReason.WATCHPOINT, id) after the accessing instruction
        completes. Peripheral-model writes are not seen."""
        return self._check(self._L.avr_host_watch_add(
            self._handle(), _uint("addr", addr, 0xFFFF), _uint("length", length, 0xFFFF),
            int(flags)))

    def remove_watchpoint(self, wp: int) -> None:
        self._check(self._L.avr_host_watch_remove(self._handle(), wp))

    # -- pins ----------------------------------------------------------------

    def _pin_table(self) -> frozendict:
        L, h = self._L, self._h
        names = {}
        for pin in range(L.avr_host_pin_count(h)):
            if (n := L.avr_host_pin_name(h, pin)) != ffi.NULL:
                names[ffi.string(n).decode()] = pin
        return frozendict(names)

    @property
    def pins(self) -> frozendict:
        """Pin name -> id for every fitted port bit, e.g. {"PA0": 0, ...}."""
        return self._pins

    def pin(self, ref: PinRef) -> int:
        if isinstance(ref, int) and not isinstance(ref, bool):
            if not 0 <= ref <= _INT_MAX:
                raise NotFound(-2, f"no pin {ref}")
            return ref
        try:
            return self._pins[ref.upper()]
        except KeyError:
            raise NotFound(-2, f"no pin {ref!r}") from None

    def pin_state(self, ref: PinRef) -> PinState:
        p = ffi.new("avr_host_pin_t *")
        self._check(self._L.avr_host_pin_get(self._handle(), self.pin(ref), p))
        return PinState(bool(p.dir), bool(p.out), bool(getattr(p, "in")),
                        bool(p.pullup), bool(p.driven), bool(p.floating))

    def drive(self, ref: PinRef, level: bool) -> None:
        """Drive an input pin from outside (button, external part)."""
        self._check(self._L.avr_host_pin_drive(self._handle(), self.pin(ref),
                                               int(bool(level))))

    def release(self, ref: PinRef) -> None:
        self._check(self._L.avr_host_pin_release(self._handle(), self.pin(ref)))

    # -- attached parts ------------------------------------------------------

    def attach_oled(self, bus: str = "twi", instance: int = 0, i2c_addr: int = 0x3C,
                    cs: PinRef | None = None, dc: PinRef | None = None,
                    res: PinRef | None = None) -> int:
        """Wire a native SSD1306 (128x64) to TWI<instance> (`i2c_addr` 0x3C/0x3D)
        or 4-wire SPI<instance> (`dc` required, `cs` optional: none = always
        selected). `res` is an optional RES# pin whose falling edge resets the
        display. The bus pins themselves are the firmware's business. Returns
        the id for `read_oled`; the part lives until the simulator closes."""
        if bus not in ("twi", "spi"):
            raise InvalidArgument(-1, "bus must be 'twi' or 'spi'")
        cfg = ffi.new("avr_host_oled_cfg_t *")
        cfg.bus = 1 if bus == "twi" else 2
        cfg.instance = _uint("instance", instance, 9)
        cfg.i2c_addr = _uint("i2c_addr", i2c_addr, 255)
        cfg.cs_pin = -1 if cs is None else self.pin(cs)
        cfg.dc_pin = -1 if dc is None else self.pin(dc)
        cfg.res_pin = -1 if res is None else self.pin(res)
        return self._check(self._L.avr_host_oled_attach(self._handle(), cfg))

    def detach_oled(self, oled: int) -> None:
        self._check(self._L.avr_host_oled_detach(self._handle(), oled))

    def read_oled(self, oled: int) -> OledState:
        o = ffi.new("avr_host_oled_t *")
        self._check(self._L.avr_host_oled_read(self._handle(), oled, o))
        return OledState(o.seq, bool(o.on), bool(o.all_on), bool(o.inverse), bool(o.remap),
                         bool(o.com_flip), o.contrast, o.start_line, o.addr_mode,
                         bytes(ffi.buffer(o.vram)))

    # -- serial --------------------------------------------------------------

    def uart_send(self, index: int, data: Buffer | str) -> None:
        if isinstance(data, str):
            data = data.encode()
        buf = ffi.from_buffer("uint8_t[]", data)
        self._check(self._L.avr_host_uart_send(self._handle(), index, buf,
                                               len(buf)))

    # -- events --------------------------------------------------------------

    def subscribe_pin(self, ref: PinRef, coalesce_cycles: int = 0) -> int:
        return self._check(self._L.avr_host_subscribe_pin(
            self._handle(), self.pin(ref),
            _uint("coalesce_cycles", coalesce_cycles)))

    def subscribe_uart(self, index: int) -> int:
        return self._check(self._L.avr_host_subscribe_uart(self._handle(), index))

    def subscribe_interrupts(self) -> int:
        return self._check(self._L.avr_host_subscribe_interrupts(self._handle()))

    def subscribe_irq(self, ioctl: int, irq_index: int) -> int:
        return self._check(self._L.avr_host_subscribe_irq(self._handle(), ioctl,
                                                          irq_index))

    def unsubscribe(self, sub: int) -> None:
        self._check(self._L.avr_host_unsubscribe(self._handle(), sub))

    def poll_events(self, max_events: int = 256) -> list[Event]:
        """Drain up to `max_events` queued events (never blocks)."""
        if not 1 <= max_events <= _INT_MAX:
            raise InvalidArgument(-1, "max_events must be >= 1")
        buf = ffi.new("avr_host_event_t[]", max_events)
        n = self._check(self._L.avr_host_poll_events(self._handle(), buf,
                                                     max_events))
        return [_event(buf[i]) for i in range(n)]

    def events(self, limit: int = 16384) -> Iterator[Event]:
        """Queued events, oldest first: drains until the queue runs dry, but
        stops after `limit` events so a running simulation that produces
        events as fast as they are read can't keep this going forever."""
        while limit > 0:
            batch = self.poll_events(min(256, limit))
            yield from batch
            limit -= len(batch)
            if len(batch) < 256:
                return

    def fileno(self) -> int:
        """Readable while events are pending (for select / QSocketNotifier).
        Don't read or close it."""
        return self._L.avr_host_event_fd(self._handle())

    def wait_events(self, timeout: float | None = None) -> list[Event]:
        """Block until events are pending (or timeout), then drain them."""
        ready, _, _ = select.select([self.fileno()], [], [], timeout)
        return list(self.events()) if ready else []

    # -- escape hatch --------------------------------------------------------

    @property
    def raw_avr(self):
        """The native avr_t* (UNSTABLE; see avr_host_raw_avr())."""
        return self._L.avr_host_raw_avr(self._handle())
