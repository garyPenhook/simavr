"""Tests for simavr_py. Host-only like tests/test_sim_host.c: firmware is
hand-assembled, so no avr-gcc is needed (encodings and atmega4809 addresses
as used by the C tests)."""

import re
import shutil
import subprocess
import sys
import threading
from pathlib import Path

import pytest

import simavr_py as sa
from simavr_py._ffi import CDEF, ffi

REPO = Path(__file__).resolve().parents[2]
HEADER = REPO / "simavr" / "sim" / "sim_host.h"

PORTA_DIR, PORTA_IN = 0x0400, 0x0408
USART0 = 0x0800
U_RXDATAL, U_TXDATAL, U_STATUS, U_CTRLB = 0x00, 0x02, 0x04, 0x06
U_RXEN, U_TXEN, U_RXCIF = 0x80, 0x40, 0x80


def words(*ops: int) -> bytes:
    return b"".join(op.to_bytes(2, "little") for op in ops)


def rjmp(k: int) -> int:
    return 0xC000 | (k & 0x0FFF)


# LDI r16,1 ; STS PORTA.DIR,r16 ; SBI VPORTA.OUT,0 ; CBI VPORTA.OUT,0 ; RJMP 0x06
BLINK = words(0xE001, 0x9300, PORTA_DIR, 0x9A08, 0x9808, rjmp(-3))


@pytest.fixture
def sim():
    with sa.Simulator("atmega4809") as s:
        s.load_flash(BLINK)
        yield s


# ---------------------------------------------------------------- bindings


def _header_functions() -> set[str]:
    return set(re.findall(r"^(avr_host_\w+)\(", HEADER.read_text(), re.M))


def test_cdef_matches_header():
    declared = set(re.findall(r"\b(avr_host_\w+)\s*\(", CDEF))
    assert declared == _header_functions()


@pytest.mark.skipif(not shutil.which("cc"), reason="no C compiler")
def test_struct_layout_matches_c(tmp_path):
    fields = {
        "avr_host_cpu_t": ["pc", "sp", "sreg", "r", "cycle"],
        "avr_host_pin_t": ["dir", "out", "in", "pullup", "driven", "floating"],
        "avr_host_event_t": ["type", "sub", "cycle", "a", "b", "c"],
        "avr_host_reg_info_t": ["name", "peripheral", "addr", "size", "reset"],
    }
    prints = []
    for t, fs in fields.items():
        prints.append(f'printf("{t} %zu\\n", sizeof({t}));')
        prints += [f'printf("{t}.{f} %zu\\n", offsetof({t}, {f}));' for f in fs]
    src = tmp_path / "layout.c"
    src.write_text("#include <stdio.h>\n#include <stddef.h>\n"
                   f'#include "{HEADER}"\nint main(void) {{ {" ".join(prints)} }}\n')
    exe = tmp_path / "layout"
    subprocess.run(["cc", "-o", str(exe), str(src)], check=True)
    c_layout = dict(line.split() for line in
                    subprocess.run([str(exe)], capture_output=True, text=True,
                                   check=True).stdout.splitlines())
    for t, fs in fields.items():
        assert int(c_layout[t]) == ffi.sizeof(t), t
        for f in fs:
            assert int(c_layout[f"{t}.{f}"]) == ffi.offsetof(t, f), f"{t}.{f}"


def test_import_is_lazy():
    code = ("import sys, simavr_py; "
            "assert 'cffi' not in sys.modules and 'simavr_py.core' not in sys.modules; "
            "simavr_py.Simulator; "
            "assert 'simavr_py.core' in sys.modules; print('ok')")
    out = subprocess.run([sys.executable, "-c", code], capture_output=True,
                         text=True)
    assert out.stdout.strip() == "ok", out.stderr


# ---------------------------------------------------------------- lifecycle


def test_discovery():
    assert sa.api_version() == 0
    assert "atmega4809" in sa.cores()


def test_unknown_core():
    with pytest.raises(sa.NotFound, match="atmega9999"):
        sa.Simulator("atmega9999")


def test_missing_elf():
    with pytest.raises(sa.LoadError):
        sa.Simulator.from_elf("/nonexistent.elf", "atmega4809")


def test_close_is_idempotent_and_final():
    s = sa.Simulator("atmega4809")
    assert repr(s).startswith("<Simulator atmega4809 PAUSED")
    s.close()
    s.close()
    assert s.closed
    with pytest.raises(sa.StateError):
        s.step()


# ---------------------------------------------------------------- manual run


def test_step_cpu_and_pins(sim):
    assert sim.state is sa.State.PAUSED
    with pytest.raises(sa.StateError):
        sim.resume()                       # needs start()
    sim.step()
    assert sim.cpu.pc == 2 and sim.cpu.r[16] == 1
    sim.step()
    assert sim.cpu.pc == 6
    assert sim.pins["PA0"] == 0 and "PA7" in sim.pins
    assert sim.pin_state("pa0").dir
    sub = sim.subscribe_pin("PA0")
    sim.step()
    (ev,) = [e for e in sim.events() if isinstance(e, sa.PinEvent)]
    assert ev.sub == sub and ev.pin == 0 and ev.level
    with pytest.raises(sa.NotFound):
        sim.pin("PZ9")


def test_breakpoint_events(sim):
    bp = sim.add_breakpoint(0x08)
    sim.run_cycles(1000)
    assert sim.cpu.pc == 0x08
    states = [e for e in sim.events() if isinstance(e, sa.StateEvent)]
    assert states[-1].reason is sa.StopReason.BREAKPOINT and states[-1].id == bp
    sim.remove_breakpoint(bp)
    with pytest.raises(sa.NotFound):
        sim.remove_breakpoint(bp)


def test_cpu_roundtrip(sim):
    cpu = sim.cpu
    sim.cpu = sa.Cpu(pc=0x10, sp=cpu.sp, sreg=0x81, r=bytes(range(32)))
    got = sim.cpu
    assert (got.pc, got.sreg, got.r) == (0x10, 0x81, bytes(range(32)))
    with pytest.raises(sa.OutOfRange):
        sim.cpu = sa.Cpu(pc=0x11, sp=cpu.sp, sreg=0, r=bytes(32))


def test_memory(sim):
    assert sim.mem_size(sa.Space.FLASH) > 0
    sim.write(sa.Space.EEPROM, 0, b"\x5a\xa5")
    sim.reset()
    assert sim.read(sa.Space.EEPROM, 0, 2) == b"\x5a\xa5"
    assert sim.read(sa.Space.FLASH, 0, 2) == BLINK[:2]
    with pytest.raises(sa.OutOfRange):
        sim.read(sa.Space.FLASH, sim.mem_size(sa.Space.FLASH) - 1, 2)


def test_drive_input_pin(sim):
    sim.drive("PA1", True)
    assert sim.pin_state("PA1").level and sim.pin_state("PA1").driven
    assert sim.peek(PORTA_IN, side_effects=True) & 0x02
    sim.release("PA1")
    assert not sim.pin_state("PA1").driven


def test_uart(sim):
    sim.poke(USART0 + U_CTRLB, U_RXEN | U_TXEN)
    sub = sim.subscribe_uart(0)
    sim.poke(USART0 + U_TXDATAL, ord("H"))
    tx = [e for e in sim.events() if isinstance(e, sa.UartTxEvent)]
    assert [(e.sub, e.byte) for e in tx] == [(sub, ord("H"))]
    sim.uart_send(0, "héllo")              # str is sent as UTF-8
    got = bytearray()
    while sim.peek(USART0 + U_STATUS) & U_RXCIF:
        got.append(sim.peek(USART0 + U_RXDATAL, side_effects=True))
    assert got.decode() == "héllo"


# ---------------------------------------------------------------- threaded


def test_threaded_run(sim):
    sub = sim.subscribe_pin("PA0", coalesce_cycles=1000)
    sim.start()
    sim.resume()
    assert sim.state is sa.State.RUNNING
    assert not sim.wait_stopped(timeout=0.02)
    with pytest.raises(sa.StateError):
        sim.cpu
    evs = sim.wait_events(timeout=1.0)
    assert any(isinstance(e, sa.PinEvent) and e.sub == sub for e in evs)
    sim.pause()
    assert sim.wait_stopped(timeout=1.0)
    assert sim.state is sa.State.PAUSED


def test_concurrent_access(sim):
    """Several Python threads using one instance while it runs: exercises
    the GIL-free path on the free-threaded build."""
    sim.start()
    sim.resume()
    errors = []

    def worker():
        try:
            for _ in range(500):
                sim.peek(PORTA_DIR)
                sim.pin_state("PA0")
                sim.poll_events(16)
        except Exception as e:             # pragma: no cover - reported below
            errors.append(e)

    c0 = sim.cycle
    threads = [threading.Thread(target=worker) for _ in range(4)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    sim.pause()
    sim.wait_stopped(timeout=1.0)
    assert not errors
    assert sim.cycle > c0


def test_free_threaded_build_keeps_gil_off():
    if not hasattr(sys, "_is_gil_enabled") or sys._is_gil_enabled():
        pytest.skip("not a free-threaded interpreter")
    sa.cores()                             # loads cffi + libsimavr
    assert not sys._is_gil_enabled(), "an import re-enabled the GIL"


# ---------------------------------------------------------------- review fixes


def test_constructor_error_classes():
    with pytest.raises(sa.NotFound):
        sa.Simulator("atmega9999")
    with pytest.raises(sa.InvalidArgument):
        sa.Simulator("atmega4809", frequency=-1)


def test_argument_validation(sim):
    for call in (lambda: sim.step(-1), lambda: sim.run_cycles(-5),
                 lambda: sim.read(sa.Space.FLASH, -1, 1),
                 lambda: sim.read(sa.Space.FLASH, 0, 1 << 40),
                 lambda: sim.add_breakpoint(1 << 33),
                 lambda: sim.poll_events(0)):
        with pytest.raises(sa.InvalidArgument):
            call()
    with pytest.raises(sa.NotFound):
        sim.pin_state(1 << 40)
    assert sim.wait_stopped(timeout=1e9)   # paused: returns at once, no overflow


def test_stop_events_queued_before_wait_returns(sim):
    sim.start()
    for _ in range(20):
        sim.resume()
        sim.pause()
        assert sim.wait_stopped(timeout=1.0)
        states = [e for e in sim.events() if isinstance(e, sa.StateEvent)]
        assert states and states[-1].state is sa.State.PAUSED


def test_events_bounded_while_running(sim):
    sim.subscribe_pin("PA0")               # uncoalesced: an event per edge
    sim.start()
    sim.resume()
    got = list(sim.events(limit=5000))
    assert len(got) <= 5000
    assert sim.wait_events(timeout=0.5) is not None   # returns, doesn't spin
    sim.pause()
    sim.wait_stopped(timeout=1.0)


def test_error_messages_are_per_thread(sim):
    sim.start()
    sim.resume()
    barrier = threading.Barrier(2)
    results = {}

    def bad_breakpoint():
        barrier.wait()
        for _ in range(300):
            try:
                sim.remove_breakpoint(99)
            except sa.NotFound as e:
                if "breakpoint" not in str(e):
                    results["bp"] = str(e)
                    return

    def cpu_while_running():
        barrier.wait()
        for _ in range(300):
            try:
                sim.cpu
            except sa.StateError as e:
                if "pause" not in str(e):
                    results["cpu"] = str(e)
                    return

    threads = [threading.Thread(target=bad_breakpoint),
               threading.Thread(target=cpu_while_running)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    sim.pause()
    sim.wait_stopped(timeout=1.0)
    assert not results, f"cross-thread error messages: {results}"
