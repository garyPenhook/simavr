"""System-block decoding for every modern layout (no Qt, no firmware), then the System dock against
real firmware."""

import os
import subprocess
import sys

from simavr_py.registers import registers
from simavr_py.system import KINDS, decode, system

CORES = ["atmega4809", "attiny3217", "attiny3224", "attiny1614", "attiny412"]


def rows(mmcu, mem=None):
    regs = {r.name: r.address for r in registers(mmcu)}
    by_addr = {regs[n]: v for n, v in (mem or {}).items()}
    return {(b.name, reg): (value, fields) for b in system(mmcu)
            for reg, value, fields in decode(mmcu, b, lambda a, s: by_addr.get(a, 0))}


def test_unknown_device_has_none():
    assert system("atmega328") == () and system("") == ()


def test_every_core_lists_the_blocks():
    for m in CORES:
        names = [b.name for b in system(m)]
        assert names == [k for k in KINDS if k in names], m
        assert {"RSTCTRL", "SLPCTRL", "WDT", "CPUINT", "RTC", "CCL", "NVMCTRL"} <= set(names), m


def test_fields():
    for m in CORES:
        r = rows(m, {"RTC.CLKSEL": 1, "RTC.PITCTRLA": 0x01 | (9 << 3), "SLPCTRL.CTRLA": 0x03,
                     "CPUINT.STATUS": 0x81})
        assert "CLKSEL=INT1K" in r["RTC", "CLKSEL"][1], m
        assert "PITEN" in r["RTC", "PITCTRLA"][1] and "PERIOD=CYC" in r["RTC", "PITCTRLA"][1], m
        assert "SEN" in r["SLPCTRL", "CTRLA"][1], m
        assert "NMIEX" in r["CPUINT", "STATUS"][1] and "LVL0EX" in r["CPUINT", "STATUS"][1], m


SCRIPT = """
import sys
from PySide6.QtWidgets import QApplication
from simavr_py.gui.window import MainWindow

app = QApplication([])
win = MainWindow("attiny3217", 3333333, speed=0)
win.load(sys.argv[1])
win.run(); win.pause(); assert win._sim.wait_stopped(5)
win.refresh()
p = win.syspane
assert p._vdd.isHidden()
rows = {(p._table.item(i, 0).text(), p._table.item(i, 1).text()): p._table.item(i, 3).text()
        for i in range(p._table.rowCount())}
assert "PITEN" in rows["RTC", "PITCTRLA"], rows
win.close()
"""


def test_system_pane(tmp_path):
    src = tmp_path / "t.c"
    src.write_text("#include <avr/io.h>\nint main(void){RTC.PITCTRLA = RTC_PITEN_bm; for(;;);}\n")
    elf = tmp_path / "t.elf"
    r = subprocess.run(["avr-gcc", "-mmcu=attiny3217", "-Os", "-o", elf, src],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    env = {**os.environ, "QT_QPA_PLATFORM": "offscreen"}
    r = subprocess.run([sys.executable, "-c", SCRIPT, str(elf)], env=env, capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
