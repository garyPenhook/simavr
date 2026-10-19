"""Offscreen smoke test of the Qt shell (skipped without PySide6 or avr-gcc).

Runs in a subprocess: on Python 3.15.0a7, importing PySide6 stops the module
attribute access in `simavr_py` from reifying its lazy imports, which would
break test_bindings.py if both shared a process.
"""

import importlib.util
import os
import shutil
import subprocess
import sys

import pytest

if importlib.util.find_spec("PySide6") is None:
    pytest.skip("PySide6 not installed", allow_module_level=True)

FIRMWARE = r"""
#include <avr/io.h>
int main(void) {
    PORTA.DIRSET = 1;
    USART0.BAUD = 1;
    USART0.CTRLB = USART_TXEN_bm;
    for (;;) {
        PORTA.OUTTGL = 1;
        while (!(USART0.STATUS & USART_DREIF_bm)) ;
        USART0.TXDATAL = 'h';
    }
}
"""

SCRIPT = r"""
import sys
from PySide6.QtWidgets import QApplication
from simavr_py.gui.window import MainWindow

app = QApplication([])
win = MainWindow("atmega4809", 3333333, speed=0)
win.load(sys.argv[1])
assert win._sim is not None
win.run()
assert win._sim.wait_events(5)            # UART bytes arrive
win._drain()
win.pause()
assert win._sim.wait_stopped(5), 'pause did not take effect'
win.refresh()
assert set(win.uart._out.toPlainText()) == {"h"}
assert win.cpu._labels["State"].text() == "PAUSED"
assert win._sim.pin_state("PA0").dir      # PA0 configured as output
assert not win._acts["pause"].isEnabled() and win._acts["step"].isEnabled()
win.close()
assert win._sim is None
"""


def test_run_pause_uart_and_pins(tmp_path):
    if not shutil.which("avr-gcc"):
        pytest.skip("avr-gcc not installed")
    src, elf = tmp_path / "fw.c", tmp_path / "fw.elf"
    src.write_text(FIRMWARE)
    r = subprocess.run(["avr-gcc", "-mmcu=atmega4809", "-Os", "-o", elf, src],
                       capture_output=True)
    if r.returncode:
        pytest.skip("avr-gcc lacks atmega4809 support")
    env = {**os.environ, "QT_QPA_PLATFORM": "offscreen"}
    r = subprocess.run([sys.executable, "-c", SCRIPT, str(elf)], env=env,
                       capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, r.stderr


SESSION_SCRIPT = r"""
import json, sys
from PySide6.QtWidgets import QApplication
from simavr_py.gui.window import MainWindow, parse_watch

assert parse_watch("0x40:2") == ("0x0040", 0x40, 2)
assert parse_watch("porta=405") == ("porta", 0x405, 1)
for bad in ("zz", "40:3", ""):
    try:
        parse_watch(bad)
    except ValueError:
        pass
    else:
        raise AssertionError(bad)

app = QApplication([])
win = MainWindow("atmega4809", 3333333, speed=0)
win.load(sys.argv[1])
win.watch.set_watches([parse_watch("pa_dir=0x0400"), parse_watch("nowhere=FFFF:2")])
win.pins.apply_driven({"PB1": True, "NOPE": True})
assert win.pins.driven() == {"PB1": True}
win.run(); assert win._sim.wait_events(5)
win.pause(); assert win._sim.wait_stopped(5)
win.refresh()
assert win.watch._table.item(0, 2).text() == "0x01"      # PA0 is an output
assert win.watch._table.item(1, 2).text() == "n/a"
d = json.loads(json.dumps(win.session_dict()))
win.close()

win2 = MainWindow(None, 0)
win2.apply_session(d)
assert win2._sim is not None and win2._sim.mmcu == "atmega4809"
assert win2.watch.watches == [("pa_dir", 0x400, 1), ("nowhere", 0xFFFF, 2)]
assert win2.pins.driven() == {"PB1": True}
try:
    win2.apply_session({"version": 99})
except ValueError:
    pass
else:
    raise AssertionError("bad version accepted")
win2.close()
"""


def test_watch_pane_and_session_roundtrip(tmp_path):
    if not shutil.which("avr-gcc"):
        pytest.skip("avr-gcc not installed")
    src, elf = tmp_path / "fw.c", tmp_path / "fw.elf"
    src.write_text(FIRMWARE)
    r = subprocess.run(["avr-gcc", "-mmcu=atmega4809", "-Os", "-o", elf, src],
                       capture_output=True)
    if r.returncode:
        pytest.skip("avr-gcc lacks atmega4809 support")
    env = {**os.environ, "QT_QPA_PLATFORM": "offscreen"}
    r = subprocess.run([sys.executable, "-c", SESSION_SCRIPT, str(elf)], env=env,
                       capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, r.stderr


BOARD_FIRMWARE = r"""
#include <avr/io.h>
int main(void) {
    PORTA.DIRSET = 3;
    PORTA.OUTSET = 1;                       /* PA0 lit */
    PORTB.PIN2CTRL = PORT_PULLUPEN_bm;      /* SW0 to ground, internal pull-up */
    for (;;) {
        if (PORTB.IN & 4) PORTA.OUTCLR = 2; else PORTA.OUTSET = 2;   /* PA1 = SW0 pressed */
    }
}
"""

BOARD_SCRIPT = r"""
import sys, time
from PySide6.QtWidgets import QApplication
from simavr_py.gui.window import MainWindow

app = QApplication([])
win = MainWindow(None, 0, speed=0)
win.load_board(sys.argv[2])
assert win.board.board.name == "ATmega4809 demo"
assert win._mmcu == "atmega4809" and win._freq == 3333333
win.load(sys.argv[1])
rt = win.board._runtime
assert rt is not None, "board did not bind"
assert len(win.board._leds) == 3
win.run()

def settle(pred, what):
    for _ in range(200):
        app.processEvents(); win._drain(); win.refresh()
        if pred():
            return
        time.sleep(0.02)
    raise AssertionError(what)

sim = win._sim
settle(lambda: win.board._leds[0]._level == 1.0, "LED0 never lit")
assert win.board._leds[2]._level == 1.0          # active-low LED2: PA2 low => lit
assert not sim.pin_state("PA1").out
win.board._button(3, True)                        # press SW0
settle(lambda: sim.pin_state("PA1").out, "press not seen by firmware")
settle(lambda: win.board._leds[1]._level == 1.0, "LED1 never lit")
win.board._button(3, False)                       # release
settle(lambda: not sim.pin_state("PA1").out, "release not seen by firmware")

win.board._turn(5, +1)                        # encoder is widget 5
assert win.board._pending
settle(lambda: not win.board._pending, "encoder detent never played out")
win.close()
assert win._sim is None and win.board._runtime is None
"""


def test_board_canvas_end_to_end(tmp_path):
    if not shutil.which("avr-gcc"):
        pytest.skip("avr-gcc not installed")
    src, elf = tmp_path / "fw.c", tmp_path / "fw.elf"
    src.write_text(BOARD_FIRMWARE)
    r = subprocess.run(["avr-gcc", "-mmcu=atmega4809", "-Os", "-o", elf, src],
                       capture_output=True)
    if r.returncode:
        pytest.skip("avr-gcc lacks atmega4809 support")
    board = os.path.join(os.path.dirname(__file__), "..", "boards", "demo_4809.json")
    env = {**os.environ, "QT_QPA_PLATFORM": "offscreen"}
    r = subprocess.run([sys.executable, "-c", BOARD_SCRIPT, str(elf), board], env=env,
                       capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, r.stderr


LCD_FIRMWARE = r"""
#include <avr/io.h>
/* HD44780 on PORTD: RS=PD0 E=PD1 D4..D7=PD4..PD7, write-only (no delays needed). */
static void nib(unsigned char n, unsigned char rs) {
    PORTD.OUT = (n << 4) | rs;
    PORTD.OUT = (n << 4) | rs | 2;          /* E high */
    PORTD.OUT = (n << 4) | rs;              /* E falling edge latches */
}
static void send(unsigned char b, unsigned char rs) { nib(b >> 4, rs); nib(b & 15, rs); }
int main(void) {
    PORTD.DIRSET = 0xF3;
    nib(3, 0); nib(3, 0); nib(3, 0); nib(2, 0);
    send(0x28, 0); send(0x0C, 0); send(0x06, 0); send(0x01, 0);
    send(0x40, 0);                                          /* CGRAM 0: a heart */
    static const unsigned char heart[8] = {0, 10, 31, 31, 14, 4, 0, 0};
    for (int i = 0; i < 8; i++) send(heart[i], 1);
    send(0x80, 0);
    for (const char *s = "Hi \0" "x"; *s; s++) send(*s, 1);
    send(0, 1);                                             /* custom glyph 0 */
    send(0xC0, 0);
    for (const char *s = "simavr"; *s; s++) send(*s, 1);
    for (;;) ;
}
"""

LCD_SCRIPT = r"""
import sys, time
from PySide6.QtWidgets import QApplication
from simavr_py.gui.window import MainWindow

app = QApplication([])
win = MainWindow(None, 0, speed=0)
win.load_board(sys.argv[2])
win.load(sys.argv[1])
lcd = win.board._runtime.lcds[6]            # the LCD is widget 6 of the demo board
win.run()
for _ in range(200):
    app.processEvents(); win._drain(); win.refresh()
    if lcd.lines()[1].startswith("simavr"):
        break
    time.sleep(0.02)
assert lcd.lines()[0].startswith("Hi ") and lcd.codes()[0][3] == 0, lcd.lines()
assert lcd.lines()[1] == "simavr".ljust(16)
assert not lcd.lost and lcd.glyph_rows(0)[1] == 10
assert win.board._lcds[6]._seen[0] == lcd.version      # widget repainted for it
pix = win.board._lcds[6].grab()                         # paints without error
assert not pix.isNull()
win.close()
"""


def test_lcd_end_to_end(tmp_path):
    if not shutil.which("avr-gcc"):
        pytest.skip("avr-gcc not installed")
    src, elf = tmp_path / "fw.c", tmp_path / "fw.elf"
    src.write_text(LCD_FIRMWARE)
    r = subprocess.run(["avr-gcc", "-mmcu=atmega4809", "-Os", "-o", elf, src],
                       capture_output=True)
    if r.returncode:
        pytest.skip("avr-gcc lacks atmega4809 support")
    board = os.path.join(os.path.dirname(__file__), "..", "boards", "demo_4809.json")
    env = {**os.environ, "QT_QPA_PLATFORM": "offscreen"}
    r = subprocess.run([sys.executable, "-c", LCD_SCRIPT, str(elf), board], env=env,
                       capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, r.stderr


BREAK_SCRIPT = r"""
import json, sys
from PySide6.QtWidgets import QApplication
from simavr_py.core import State
from simavr_py.gui.window import MainWindow, parse_breakpoint

assert parse_breakpoint("0x120") == 0x120 and parse_breakpoint("40") == 0x40
assert parse_breakpoint("0b10") == 0xB10
for bad in ("zz", "121", "-2", "", "1_0"):
    try:
        parse_breakpoint(bad)
    except ValueError:
        pass
    else:
        raise AssertionError(bad)

from simavr_py.symbols import Symbol, SymbolTable
st = SymbolTable([Symbol(0x100, "main", 0x40), Symbol(0x200, "add", 0x10)])
assert parse_breakpoint("main", st) == 0x100 and parse_breakpoint("main+0x4", st) == 0x104
assert parse_breakpoint("add", st) == 0x200 and parse_breakpoint("0xade", st) == 0xADE
assert parse_breakpoint("0x40", st) == 0x40
for bad in ("main+1", "main+zz", "main+-2", "nosuch"):
    try:
        parse_breakpoint(bad, st)
    except ValueError:
        pass
    else:
        raise AssertionError(bad)

app = QApplication([])
win = MainWindow("atmega4809", 3333333, speed=0)
win.breaks.set_addresses([0, 0xFFFFFE])      # second is beyond flash: listed, unarmed
win.load(sys.argv[1])
assert win.breaks._table.item(0, 1).text() == "yes"
assert win.breaks._table.item(1, 1).text() == "no"
win.run()
assert win._sim.wait_stopped(5), "reset-vector breakpoint did not fire"
win._drain()
assert win._sim.state == State.PAUSED and win.breaks._hit == 0
assert win.breaks._table.item(0, 0).background().color().name() == "#f4a6a6"
win.run()                                    # resuming from the hit PC must not refire
assert not win._sim.wait_stopped(0.3)
assert win.breaks._hit is None
win.pause(); assert win._sim.wait_stopped(5)
d = json.loads(json.dumps(win.session_dict()))
assert d["breakpoints"] == [0, 0xFFFFFE]
win.breaks._table.selectRow(0); win.breaks._remove_selected()
assert win.breaks.addresses == [0xFFFFFE]
win.close()

win2 = MainWindow(None, 0)
win2.apply_session(d)
assert win2.breaks.addresses == [0, 0xFFFFFE]
assert win2.breaks._table.item(0, 1).text() == "yes"
win2.close()
"""


def test_breakpoint_pane(tmp_path):
    if not shutil.which("avr-gcc"):
        pytest.skip("avr-gcc not installed")
    src, elf = tmp_path / "fw.c", tmp_path / "fw.elf"
    src.write_text(FIRMWARE)
    r = subprocess.run(["avr-gcc", "-mmcu=atmega4809", "-Os", "-o", elf, src],
                       capture_output=True)
    if r.returncode:
        pytest.skip("avr-gcc lacks atmega4809 support")
    env = {**os.environ, "QT_QPA_PLATFORM": "offscreen"}
    r = subprocess.run([sys.executable, "-c", BREAK_SCRIPT, str(elf)], env=env,
                       capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, r.stderr


DISASM_SCRIPT = r"""
import sys
from PySide6.QtWidgets import QApplication
from simavr_py.gui.window import MainWindow

app = QApplication([])
win = MainWindow("atmega4809", 3333333, speed=0)
win.load(sys.argv[1])
win.step(); assert win._sim.wait_stopped(5)
win.refresh()
t = win.disasm._table
pc = win._sim.cpu.pc
rows = [r for r in range(t.rowCount()) if t.item(r, 1).text() == f"0x{pc:05X}"]
assert len(rows) == 1 and t.item(rows[0], 4).text(), "PC row missing"
assert t.isEnabled()
assert any(t.item(r, 2).text() for r in range(t.rowCount())), "no symbol labels"
assert any(t.item(r, 3).text().startswith("fw.c:") for r in range(t.rowCount())), "no source lines"
t.cellDoubleClicked.emit(rows[0], 0)                 # toggle a breakpoint on the PC row
assert win.breaks.addresses == [pc]
win.refresh()
assert t.item(rows[0], 0).text() == "●"
t.cellDoubleClicked.emit(rows[0], 0)
assert win.breaks.addresses == []
src = win.source
for _ in range(200):                                  # startup code has no source line
    win.refresh()
    if "fw.c" in src._head.text():
        break
    win.step(); assert win._sim.wait_stopped(5)
assert src._head.text().split(":")[0].endswith("fw.c") and src._text.toPlainText(), src._head.text()
assert src._text.extraSelections()
cur = src._text.textCursor().block().text()
assert cur.split(None, 1)[0] == src._head.text().rsplit(":", 1)[1], cur   # highlighted = PC line
line = int(src._head.text().rsplit(":", 1)[1])
src._text.lineDoubleClicked.emit(line - 1 - (src._shown[1] - 1))   # toggle via the PC's line
assert len(win.breaks.addresses) == 1
win.refresh()
assert src._text.toPlainText().splitlines()[line - src._shown[1]].startswith("●")
src._text.lineDoubleClicked.emit(line - src._shown[1])
assert win.breaks.addresses == []
win.run(); win.refresh()
assert not t.isEnabled() and not src._text.isEnabled()   # PC unreadable while running
win.pause(); assert win._sim.wait_stopped(5)
win.close()
win.refresh()
assert t.rowCount() == 0 and src._text.toPlainText() == ""
"""


def test_disassembly_dock(tmp_path):
    if not shutil.which("avr-gcc"):
        pytest.skip("avr-gcc not installed")
    src, elf = tmp_path / "fw.c", tmp_path / "fw.elf"
    src.write_text(FIRMWARE)
    r = subprocess.run(["avr-gcc", "-mmcu=atmega4809", "-Os", "-g", "-o", elf, src],
                       capture_output=True)
    if r.returncode:
        pytest.skip("avr-gcc lacks atmega4809 support")
    env = {**os.environ, "QT_QPA_PLATFORM": "offscreen"}
    r = subprocess.run([sys.executable, "-c", DISASM_SCRIPT, str(elf)], env=env,
                       capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, r.stderr


NEXT_FIRMWARE = r"""
#include <avr/io.h>
volatile unsigned x;
__attribute__((noinline)) void bar(void) { x++; } __attribute__((noinline)) void foo(void) { bar(); x++; }
int main(void) {
    x = 1;
    foo();
    x = 2;
    for (;;) ;
}
"""

NEXT_SCRIPT = r"""
import sys, time
from PySide6.QtWidgets import QApplication
from simavr_py.gui.window import MainWindow
from simavr_py.symbols import read_elf_symbols

app = QApplication([])
win = MainWindow("atmega4809", 3333333, speed=0)
win.load(sys.argv[1])
sim = win._sim

def line():
    h = win._line_table.find(sim.cpu.pc)
    return h.line if h else None

def settle():
    assert sim.wait_stopped(5)
    end = time.time() + 5
    while win._next is not None and time.time() < end:
        app.processEvents()
        time.sleep(0.001)
    assert win._next is None, "step-over-line never finished"
    assert sim.wait_stopped(5)

win.breaks.add(read_elf_symbols(sys.argv[1]).lookup("main").addr)
win.run()
assert sim.wait_stopped(5)
for _ in range(10):                     # main's prologue
    if line() == 5:
        break
    win.step_line(); settle()
assert line() == 5, line()
win.step_line(); settle()
assert line() == 6, line()
win.step_line(); settle()
assert line() == 7, line()              # at the call to foo()
win.step_line(); settle()
assert line() == 8, line()              # ran over foo()
win.refresh()
assert win._acts["next"].isEnabled() is True
win.step_line(); settle()
assert line() == 9, line()
win.step_line()                         # `for(;;);` never leaves its line: Pause cancels it
for _ in range(50):
    app.processEvents(); time.sleep(0.001)
win.refresh()
assert win._next is not None and win._acts["pause"].isEnabled()
win.pause()
assert win._next is None and sim.wait_stopped(5) and line() == 9
win.close()
"""

OUT_SCRIPT = r"""
import sys, time
from PySide6.QtWidgets import QApplication
from simavr_py.gui.window import MainWindow
from simavr_py.symbols import read_elf_symbols

app = QApplication([])
win = MainWindow("atmega4809", 3333333, speed=0)
win.load(sys.argv[1])
sim = win._sim
syms = read_elf_symbols(sys.argv[1])
foo, bar, main = (syms.lookup(n).addr for n in ("foo", "bar", "main"))

def settle():
    end = time.time() + 5
    while win._next is not None and time.time() < end:
        app.processEvents()
        time.sleep(0.001)
    assert win._next is None, "step-out never finished"
    assert sim.wait_stopped(5)

win.breaks.add(foo)
win.run(); assert sim.wait_stopped(5)
assert sim.cpu.pc == foo
sp_in = sim.cpu.sp
win.step_out(); settle()                # runs over bar(), stops after foo's RET
pc = sim.cpu.pc
assert pc >= main and sim.cpu.sp > sp_in, (hex(pc), hex(main))
win.close()
"""


def _build(tmp_path, source):
    src, elf = tmp_path / "fw.c", tmp_path / "fw.elf"
    src.write_text(source)
    r = subprocess.run(["avr-gcc", "-mmcu=atmega4809", "-O0", "-g", "-o", elf, src],
                       capture_output=True)
    if r.returncode:
        pytest.skip("avr-gcc lacks atmega4809 support")
    return elf


def test_step_over_line(tmp_path):
    if not shutil.which("avr-gcc"):
        pytest.skip("avr-gcc not installed")
    elf = _build(tmp_path, NEXT_FIRMWARE)
    env = {**os.environ, "QT_QPA_PLATFORM": "offscreen"}
    r = subprocess.run([sys.executable, "-c", NEXT_SCRIPT, str(elf)], env=env,
                       capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, r.stderr


def test_step_out(tmp_path):
    if not shutil.which("avr-gcc"):
        pytest.skip("avr-gcc not installed")
    elf = _build(tmp_path, NEXT_FIRMWARE)
    env = {**os.environ, "QT_QPA_PLATFORM": "offscreen"}
    r = subprocess.run([sys.executable, "-c", OUT_SCRIPT, str(elf)], env=env,
                       capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, r.stderr


TIMELINE_SCRIPT = r"""
import sys, time
from PySide6.QtWidgets import QApplication
from simavr_py.gui.window import MainWindow

app = QApplication([])
win = MainWindow("atmega4809", 3333333, speed=0)
win.load(sys.argv[1])
tl = win.timeline
assert tl.add("pa0") and not tl.add("PA0") and not tl.add("nosuchpin")
assert tl.names == ["PA0"] and tl.wanted == ["PA0"]
win.run()
end = time.time() + 5
while len(tl.traces["PA0"]) < 6 and time.time() < end:
    app.processEvents(); time.sleep(0.005)
win.pause(); assert win._sim.wait_stopped(5)
app.processEvents(); win.refresh()
assert len(tl.traces["PA0"]) >= 6, len(tl.traces["PA0"])
t0, t1 = tl.window()
assert t1 > t0 and tl.span_cycles() == t1 - t0 or t0 == 0
assert tl.grab().width() > 0                         # paints without error
tl.pan(-0.5); assert not tl._follow.isChecked()
tl.zoom(-1); tl.zoom(1)
d = win.session_dict()
assert d["timeline"] == ["PA0"]
assert tl._uart and tl.uart_bytes(0, 1 << 62)[0][1] == ord("h")
tl.place_cursor(0, 100, 500); tl.place_cursor(1, 300, 500)
assert None not in tl.cursors and tl.cursors[1] > tl.cursors[0] and "Δ" in tl._info.text()
tl.export_vcd(sys.argv[1] + ".vcd")
assert "$var wire 1 ! PA0 $end" in open(sys.argv[1] + ".vcd").read()
tl.clear(); assert len(tl.traces["PA0"]) == 1 and not tl._uart
tl.remove("PA0"); assert tl.names == [] and tl.wanted == []
win.apply_session(d, load_firmware=True)
assert tl.names == ["PA0"]                           # restored and resubscribed on load
win.close()
"""


def test_timeline_dock(tmp_path):
    if not shutil.which("avr-gcc"):
        pytest.skip("avr-gcc not installed")
    elf = _build(tmp_path, FIRMWARE)
    env = {**os.environ, "QT_QPA_PLATFORM": "offscreen"}
    r = subprocess.run([sys.executable, "-c", TIMELINE_SCRIPT, str(elf)], env=env,
                       capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, r.stderr


IRQ_FIRMWARE = r"""
#include <avr/io.h>
#include <avr/interrupt.h>
ISR(USART0_RXC_vect) { (void)USART0.RXDATAL; }
int main(void) {
    USART0.CTRLB = USART_RXEN_bm;
    USART0.CTRLA = USART_RXCIE_bm;
    sei();
    for (;;) ;
}
"""

IRQ_SCRIPT = r"""
import sys, time
from PySide6.QtWidgets import QApplication
from simavr_py.gui.window import MainWindow

app = QApplication([])
win = MainWindow("atmega4809", 3333333, speed=0)
win.load(sys.argv[1])
win.run(); time.sleep(0.2)
for _ in range(3):
    win._sim.uart_send(0, "A"); time.sleep(0.2)
win.pause(); assert win._sim.wait_stopped(5)
win._drain(); win.irqs.refresh()
st = win.irqs.stats
assert len(st) == 1, st
(vec, (level, count, cycle)), = st.items()
assert vec > 0 and level == 0 and count == 3 and cycle > 0, st
assert win.irqs._table.item(0, 1).text() == "LVL0" and win.irqs._table.item(0, 2).text() == "3"
win.irqs.clear()
assert win.irqs.stats == {} and win.irqs._table.rowCount() == 0
win.close()
"""


def test_interrupt_pane(tmp_path):
    if not shutil.which("avr-gcc"):
        pytest.skip("avr-gcc not installed")
    src, elf = tmp_path / "fw.c", tmp_path / "fw.elf"
    src.write_text(IRQ_FIRMWARE)
    r = subprocess.run(["avr-gcc", "-mmcu=atmega4809", "-Os", "-o", elf, src],
                       capture_output=True)
    if r.returncode:
        pytest.skip("avr-gcc lacks atmega4809 support")
    env = {**os.environ, "QT_QPA_PLATFORM": "offscreen"}
    r = subprocess.run([sys.executable, "-c", IRQ_SCRIPT, str(elf)], env=env,
                       capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, r.stderr


DATABREAK_SCRIPT = r"""
import json, sys
from PySide6.QtWidgets import QApplication
from simavr_py import State, WatchFlag
from simavr_py.gui.window import MainWindow, parse_data_break

assert parse_data_break("0x0407") == (0x407, 1, WatchFlag.WRITE)
assert parse_data_break("3f:2 RW") == (0x3F, 2, WatchFlag.READ | WatchFlag.WRITE)
assert parse_data_break("40 c") == (0x40, 1, WatchFlag.WRITE | WatchFlag.CHANGE)
assert parse_data_break("408 P") == (0x408, 1, WatchFlag.PERIPH)
for bad in ("", "zz", "40 x", "40:0", "10000", "1 2 3", "4_0", "40:-1"):
    try:
        parse_data_break(bad)
    except ValueError:
        continue
    raise AssertionError(f"accepted {bad!r}")

app = QApplication([])
win = MainWindow("atmega4809", 3333333, speed=0)
win.databreaks.set_items([(0x407, 1, WatchFlag.WRITE), (0xFFFF, 4, WatchFlag.WRITE)])
win.load(sys.argv[1])
t = win.databreaks._table
assert t.item(0, 3).text() == "yes" and t.item(1, 3).text() == "no"
win.run()
assert win._sim.wait_stopped(5), "watchpoint did not fire"
win._drain()
assert win._sim.state == State.PAUSED and win.databreaks._hit == (0x407, 1, WatchFlag.WRITE)
assert t.item(0, 0).background().color().name() == "#f4a6a6"
win.run()                                    # next OUTTGL comes around again
assert win._sim.wait_stopped(5)
win._drain()
assert win.databreaks._hit is not None
win.step()
assert win.databreaks._hit is None
d = json.loads(json.dumps(win.session_dict()))
assert d["watchpoints"] == [[0x407, 1, 2], [0xFFFF, 4, 2]]
t.selectRow(1); win.databreaks._remove_selected()
assert win.databreaks.items == [(0x407, 1, WatchFlag.WRITE)]
win.close()

win2 = MainWindow(None, 0)
win2.apply_session(d)
assert len(win2.databreaks.items) == 2
win2.close()
"""


def test_data_breakpoint_pane(tmp_path):
    if not shutil.which("avr-gcc"):
        pytest.skip("avr-gcc not installed")
    src, elf = tmp_path / "fw.c", tmp_path / "fw.elf"
    src.write_text(FIRMWARE)
    r = subprocess.run(["avr-gcc", "-mmcu=atmega4809", "-Os", "-o", elf, src],
                       capture_output=True)
    if r.returncode:
        pytest.skip("avr-gcc lacks atmega4809 support")
    env = {**os.environ, "QT_QPA_PLATFORM": "offscreen"}
    r = subprocess.run([sys.executable, "-c", DATABREAK_SCRIPT, str(elf)], env=env,
                       capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, r.stderr
