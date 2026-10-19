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
