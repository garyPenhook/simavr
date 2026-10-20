"""CLKCTRL decoding for every modern layout (no Qt, no firmware), then the Clock dock against
real firmware."""

import os
import subprocess
import sys

from simavr_py.clock import clkctrl, clock_chain, decode
from simavr_py.registers import registers

CORES = ["atmega4809", "attiny3217", "attiny3224", "attiny1614", "attiny412"]


def rows(mmcu, mem=None, clk=3333333):
    regs = {r.name: r.address for r in registers(mmcu)}
    by_addr = {regs[n]: v for n, v in (mem or {}).items()}
    return {reg: (value, fields) for b in clkctrl(mmcu)
            for reg, value, fields in decode(mmcu, b, lambda a, s: by_addr.get(a, 0), clk)}


def test_unknown_device_has_none():
    assert clkctrl("atmega328") == () and clkctrl("") == ()


def test_every_core_decodes():
    for m in CORES:
        assert [b.name for b in clkctrl(m)] == ["CLKCTRL"], m
        r = rows(m, {"CLKCTRL.MCLKCTRLB": 0x11})
        assert "PDIV=6X" in r["MCLKCTRLB"][1] and "PEN" in r["MCLKCTRLB"][1], m
        assert r["Main clock"][1] == "OSC20M" and r["Prescaler"][1] == "/6", m
        assert r["CLK_MAIN"][1] == "2e+07 Hz", m


def test_chain():
    assert clock_chain("OSC20M", False, "6X", 20000000) == [
        ("Main clock", "OSC20M"), ("Prescaler", "off (CLK_PER = CLK_MAIN)"),
        ("CLK_PER", "2e+07 Hz"), ("CLK_MAIN", "2e+07 Hz")]
    assert clock_chain("EXTCLK", True, "2X", 4000000)[1:] == [
        ("Prescaler", "/2"), ("CLK_PER", "4e+06 Hz"), ("CLK_MAIN", "8e+06 Hz")]
    assert clock_chain("OSC20M", True, "", 1000000) == [("Main clock", "OSC20M"), ("Prescaler", "reserved (?)")]


def test_sources():
    for sel, name in enumerate(("OSC20M", "OSCULP32K", "XOSC32K", "EXTCLK")):
        assert rows("attiny3217", {"CLKCTRL.MCLKCTRLA": sel})["Main clock"][1] == name


SCRIPT = """
import sys
from PySide6.QtWidgets import QApplication
from simavr_py.gui.window import MainWindow

app = QApplication([])
win = MainWindow("attiny3217", 3333333, speed=0)
win.load(sys.argv[1])
win.run(); win.pause(); assert win._sim.wait_stopped(5)
win.refresh()
p = win.clockpane
assert p._vdd.isHidden()
rows = {(p._table.item(i, 0).text(), p._table.item(i, 1).text()): p._table.item(i, 3).text()
        for i in range(p._table.rowCount())}
assert "PDIV=2X" in rows["CLKCTRL", "MCLKCTRLB"], rows
assert rows["CLKCTRL", "Prescaler"] == "/2", rows
assert rows["CLKCTRL", "CLK_PER"] == "1e+07 Hz" and rows["CLKCTRL", "CLK_MAIN"] == "2e+07 Hz", rows
win.close()
"""


def test_clock_pane(tmp_path):
    src = tmp_path / "t.c"
    src.write_text("#include <avr/io.h>\n#include <avr/cpufunc.h>\nint main(void){"
                   "_PROTECTED_WRITE(CLKCTRL.MCLKCTRLB, CLKCTRL_PDIV_2X_gc|CLKCTRL_PEN_bm);for(;;);}\n")
    elf = tmp_path / "t.elf"
    r = subprocess.run(["avr-gcc", "-mmcu=attiny3217", "-Os", "-o", elf, src],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    env = {**os.environ, "QT_QPA_PLATFORM": "offscreen"}
    r = subprocess.run([sys.executable, "-c", SCRIPT, str(elf)], env=env, capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
