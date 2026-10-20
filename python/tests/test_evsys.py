"""EVSYS decoding for every modern layout (no Qt, no firmware), then the Events dock against
real firmware."""

import os
import subprocess
import sys

import pytest

from simavr_py.evsys import evsys, rows
from simavr_py.registers import registers

CORES = ["atmega4809", "attiny3217", "attiny3224", "attiny1614", "attiny412"]


def test_unknown_device_has_no_evsys():
    assert evsys("atmega328") is None and evsys("") is None and rows("atmega328", lambda a: 0) == ()


@pytest.mark.parametrize("mmcu", CORES)
def test_all_off_at_reset(mmcu):
    r = rows(mmcu, lambda a: 0)
    assert r and {m for *_, m in r} == {"OFF"}
    assert {k for k, *_ in r} == {"Channel", "User"}


def test_layouts():
    assert evsys("atmega4809").layout == "channel" and len(evsys("atmega4809").channels) == 8
    assert len(evsys("atmega4809").users) == 24             # DS40002173C 14.5.3
    assert evsys("attiny3217").layout == "async" and len(evsys("attiny3217").users) == 15
    assert evsys("attiny3224").layout == "channel"


def test_mega_user_wiring():
    regs = {r.name: r.address for r in registers("atmega4809")}
    mem = {regs["EVSYS.CHANNEL2"]: 0x42,                    # PORT0_PIN2 (PA2)
           regs["EVSYS.USERTCB0"]: 3}                       # value n -> CHANNEL(n-1)
    r = {(k, n): m for k, n, _, m in rows("atmega4809", lambda a: mem.get(a, 0))}
    assert r["Channel", "CHANNEL2"] == "PORT0_PIN2"
    assert r["User", "TCB0"] == "CHANNEL2  (PORT0_PIN2)"
    assert r["User", "TCB1"] == "OFF"


def test_tiny1_user_wiring():
    regs = {r.name: r.address for r in registers("attiny3217")}
    mem = {regs["EVSYS.ASYNCCH1"]: 0x02,                    # CCL_LUT1
           regs["EVSYS.ASYNCUSER0"]: 4,                     # 3.. = ASYNCCH0.. -> ASYNCCH1
           regs["EVSYS.SYNCUSER0"]: 2,                      # 1,2 = SYNCCH0,1
           regs["EVSYS.ASYNCUSER1"]: 9}                     # selects nothing
    r = {(k, n): m for k, n, _, m in rows("attiny3217", lambda a: mem.get(a, 0))}
    assert r["Channel", "ASYNCCH1"] == "CCL_LUT1"
    assert r["User", "TCB0"] == "ASYNCCH1  (CCL_LUT1)"
    assert r["User", "TCA0"].startswith("SYNCCH1")
    assert r["User", "ADC0"] == "?"


SCRIPT = """
import sys
from PySide6.QtWidgets import QApplication
from simavr_py.gui.window import MainWindow

app = QApplication([])
win = MainWindow("atmega4809", 3333333, speed=0)
win.load(sys.argv[1])
win.run(); win.pause(); assert win._sim.wait_stopped(5)
win.refresh()
p = win.evpane
rows = {(p._table.item(i, 0).text(), p._table.item(i, 1).text()): p._table.item(i, 3).text()
        for i in range(p._table.rowCount())}
assert rows["Channel", "CHANNEL0"] == "PORT0_PIN2", rows
assert rows["User", "TCB0"] == "CHANNEL0  (PORT0_PIN2)", rows
assert rows["User", "TCB1"] == "OFF", rows
win.close()
"""


def test_events_pane(tmp_path):
    src = tmp_path / "t.c"
    src.write_text("#include <avr/io.h>\nint main(void){"
                   "EVSYS.CHANNEL0=EVSYS_GENERATOR_PORT0_PIN2_gc;"
                   "EVSYS.USERTCB0=EVSYS_CHANNEL_CHANNEL0_gc;for(;;);}\n")
    elf = tmp_path / "t.elf"
    r = subprocess.run(["avr-gcc", "-mmcu=atmega4809", "-Os", "-o", elf, src],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    env = {**os.environ, "QT_QPA_PLATFORM": "offscreen"}
    r = subprocess.run([sys.executable, "-c", SCRIPT, str(elf)], env=env, capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
