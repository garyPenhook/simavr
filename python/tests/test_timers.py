"""TCA/TCB/TCD decoding for every modern core (no Qt, no firmware), then the Timers dock
against real firmware."""

import os
import subprocess
import sys

import pytest

from simavr_py.registers import registers
from simavr_py.timers import decode, timers

CORES = ["atmega4809", "attiny3217", "attiny3224", "attiny1614", "attiny412"]


def test_unknown_device_has_no_timers():
    assert timers("atmega328") == () and timers("") == ()


@pytest.mark.parametrize("mmcu", CORES)
def test_decode_every_instance(mmcu):
    ts = timers(mmcu)
    assert ts and ts[0].name == "TCA0"
    for t in ts:
        rows = dict(decode(mmcu, t, lambda a, s: 0))
        assert rows["Enabled"] == "no"
        assert rows["Clock"] not in ("?", "")


def test_instances():
    assert [t.name for t in timers("atmega4809")] == ["TCA0", "TCB0", "TCB1", "TCB2", "TCB3"]
    assert [t.name for t in timers("attiny3217")] == ["TCA0", "TCB0", "TCB1", "TCD0"]


def test_decode_tca_tcb_values():
    regs = {r.name: r for r in registers("atmega4809")}
    mem = {regs["TCA0.SINGLE.CTRLA"].address: 0x07,     # CLKSEL=DIV8 (3<<1), ENABLE
           regs["TCA0.SINGLE.CTRLB"].address: 0x13,     # WGMODE=SINGLESLOPE, CMP0EN
           regs["TCA0.SINGLE.CNT"].address: 0x34, regs["TCA0.SINGLE.CNT"].address + 1: 0x12,
           regs["TCB0.CTRLA"].address: 0x05,            # CLKSEL=CLKTCA, ENABLE
           regs["TCB0.CTRLB"].address: 0x17}            # CNTMODE=PWM8, CCMPEN
    read = lambda a, s: int.from_bytes(bytes(mem.get(a + i, 0) for i in range(s)), "little")
    ts = {t.name: t for t in timers("atmega4809")}
    a = dict(decode("atmega4809", ts["TCA0"], read))
    assert (a["Enabled"], a["Clock"], a["Mode"], a["Outputs enabled"], a["CNT"]) == \
        ("yes", "DIV8", "SINGLESLOPE", "CMP0EN", "4660")
    b = dict(decode("atmega4809", ts["TCB0"], read))
    assert (b["Enabled"], b["Clock"], b["Mode"], b["Output pin"]) == ("yes", "CLKTCA", "PWM8", "yes")


def test_tca_split_mode():
    regs = {r.name: r for r in registers("attiny3217")}
    mem = {regs["TCA0.SINGLE.CTRLD"].address: 1, regs["TCA0.SPLIT.CTRLB"].address: 0x11,
           regs["TCA0.SPLIT.LPER"].address: 7, regs["TCA0.SPLIT.HPER"].address: 9}
    read = lambda a, s: mem.get(a, 0)
    rows = dict(decode("attiny3217", timers("attiny3217")[0], read))
    assert rows["Mode"].startswith("SPLIT") and rows["Outputs enabled"] == "LCMP0EN,HCMP0EN"
    assert rows["LPER / HPER"] == "7 / 9"


SCRIPT = """
import sys
from PySide6.QtWidgets import QApplication
from simavr_py.gui.window import MainWindow

app = QApplication([])
win = MainWindow("atmega4809", 3333333, speed=0)
win.load(sys.argv[1])
win.run(); win.pause(); assert win._sim.wait_stopped(5)
win.refresh()
p = win.timerpane
rows = {(p._table.item(i, 0).text(), p._table.item(i, 1).text()): p._table.item(i, 2).text()
        for i in range(p._table.rowCount())}
assert rows["TCA0", "Enabled"] == "yes" and rows["TCA0", "Clock"] == "DIV8", rows
assert rows["TCA0", "Mode"] == "SINGLESLOPE" and rows["TCA0", "PER"] == "999", rows
assert rows["TCA0", "Outputs enabled"] == "CMP1EN" and rows["TCA0", "CMP1"].startswith("250"), rows
assert rows["TCB0", "Enabled"] == "yes" and rows["TCB0", "Mode"] == "INT", rows
assert rows["TCB0", "CCMP"] == "500", rows
win.close()
"""


def test_timer_pane(tmp_path):
    src = tmp_path / "t.c"
    src.write_text("#include <avr/io.h>\nint main(void){"
                   "TCA0.SINGLE.PER=999;TCA0.SINGLE.CMP1=250;"
                   "TCA0.SINGLE.CTRLB=TCA_SINGLE_CMP1EN_bm|TCA_SINGLE_WGMODE_SINGLESLOPE_gc;"
                   "TCA0.SINGLE.CTRLA=TCA_SINGLE_CLKSEL_DIV8_gc|TCA_SINGLE_ENABLE_bm;"
                   "TCB0.CCMP=500;TCB0.CTRLA=TCB_ENABLE_bm;for(;;);}\n")
    elf = tmp_path / "t.elf"
    r = subprocess.run(["avr-gcc", "-mmcu=atmega4809", "-Os", "-o", elf, src],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    env = {**os.environ, "QT_QPA_PLATFORM": "offscreen"}
    r = subprocess.run([sys.executable, "-c", SCRIPT, str(elf)], env=env, capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
