"""PORTMUX decoding for every modern core (no Qt, no firmware), then the PORTMUX dock against
real firmware that changes the routing."""

import os
import subprocess
import sys

import pytest

from simavr_py.core import cores
from simavr_py.portmux import routes
from simavr_py.registers import registers

MODERN = [c for c in cores() if registers(c)]


def by_name(mmcu):
    return {f.name: f for f in routes(mmcu)}


def test_every_modern_core_has_routes():
    assert len(MODERN) == 46
    for c in MODERN:
        r = routes(c)
        assert r, c
        regs = {x.name: x for x in registers(c)}
        for f in r:
            assert f.register in regs and regs[f.register].address == f.address, (c, f.name)
            assert f.mask and f.mask >> f.shift << f.shift == f.mask, (c, f.name)
            vals = [v for v, _, _ in f.options]
            assert len(set(vals)) == len(vals), (c, f.name)
            assert all(v << f.shift & ~f.mask == 0 for v in vals), (c, f.name)
        assert [(f.address, f.shift) for f in r] == sorted((f.address, f.shift) for f in r)


def test_atmega4809_matches_datasheet():
    # DS40002173C 15.3: USARTROUTEA at offset 0x02 (USART3[1:0] .. USART0[1:0]), TCAROUTEA 0x04.
    f = by_name("atmega4809")
    u1 = f["USART1"]
    assert (u1.register, u1.address, u1.mask, u1.shift) == ("PORTMUX.USARTROUTEA", 0x05E2, 0x0C, 2)
    assert u1.decode(0x04) == (1, "ALT1", "USART1 on PC[7:4]")
    assert u1.decode(0x0C) == (3, "NONE", "Not connected to any pins")
    assert u1.decode(0x08) == (2, "reserved", "")          # 0x2 is reserved
    assert f["USART0"].decode(0x00)[2] == "USART0 on PA[3:0]"
    t = f["TCA0"]
    assert (t.address, t.mask) == (0x05E4, 0x07) and t.decode(0x05)[1:] == ("PORTF", "TCA0 pins on PF[5:0]")
    assert f["TCB3"].decode(0x08) == (1, "ALT1", "WO on PC1")      # header wording; DS: "TCB3 on PC1"
    assert [x.name for x in routes("atmega4809")].count("TCB2") == 1


def test_tinyavr1_matches_datasheet():
    # DS40002205A 15.2: CTRLA LUT1/LUT0 bits 5/4, EVOUT2..0 bits 2..0 enable the outputs;
    # CTRLB TWI0 bit 4, SPI0 bit 2, USART0 bit 0; CTRLD TCB1/TCB0.
    f = by_name("attiny3217")
    assert [(f[n].register, f[n].mask) for n in ("LUT0", "LUT1", "EVOUT2")] == [
        ("PORTMUX.CTRLA", 0x10), ("PORTMUX.CTRLA", 0x20), ("PORTMUX.CTRLA", 0x04)]
    assert f["EVOUT2"].decode(0x04)[1] == "ON" and f["EVOUT2"].decode(0)[1] == "OFF"
    assert [(f[n].mask, f[n].address) for n in ("TWI0", "SPI0", "USART0")] == [
        (0x10, 0x0201), (0x04, 0x0201), (0x01, 0x0201)]
    assert f["TCB1"].decode(0x02)[:2] == (1, "ALTERNATE")


def test_smaller_package_offers_fewer_options():
    assert len(by_name("attiny3224")["USART1"].options) == 2     # DEFAULT, NONE
    assert len(by_name("atmega4809")["USART1"].options) == 3


@pytest.mark.parametrize("mmcu", ["atmega328", "attiny85", "", "nonsense"])
def test_unknown_or_classic_is_empty(mmcu):
    assert routes(mmcu) == ()


SCRIPT = """
import sys
from PySide6.QtWidgets import QApplication
from simavr_py.gui.window import MainWindow

app = QApplication([])
win = MainWindow("atmega4809", 3333333, speed=0)
win.load(sys.argv[1])
win.run(); win.pause(); assert win._sim.wait_stopped(5)
win.refresh()
pm = win.portmux
t = pm._table
names = [r.name for r in pm._routes]
def row(n):
    i = names.index(n)
    return [t.item(i, c).text() for c in range(5)]
assert row("USART0") == ["USART0", "USARTROUTEA", "1", "ALT1", "USART0 on PA[7:4]"], row("USART0")
assert row("USART1")[2:4] == ["0", "DEFAULT"]
assert row("TCA0")[3:] == ["PORTC", "TCA0 pins on PC[5:0]"], row("TCA0")
assert row("TCB2")[2:4] == ["1", "ALT1"], row("TCB2")
assert row("TCB0")[2:4] == ["0", "DEFAULT"]
# a poke while paused (same cycle) shows up at once
win._sim.write(__import__("simavr_py.core", fromlist=["Space"]).Space.DATA, 0x05E2, bytes([0x0C]), side_effects=True)
win.refresh()
assert row("USART0")[3] == "DEFAULT" and row("USART1")[3] == "NONE", (row("USART0"), row("USART1"))
win.close()
"""


def test_portmux_pane(tmp_path):
    src = tmp_path / "m.c"
    src.write_text("#include <avr/io.h>\nint main(void){PORTMUX.USARTROUTEA=PORTMUX_USART0_ALT1_gc;"
                   "PORTMUX.TCAROUTEA=PORTMUX_TCA0_PORTC_gc;PORTMUX.TCBROUTEA=PORTMUX_TCB2_ALT1_gc;"
                   "for(;;);}\n")
    elf = tmp_path / "m.elf"
    r = subprocess.run(["avr-gcc", "-mmcu=atmega4809", "-Os", "-o", elf, src],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    env = {**os.environ, "QT_QPA_PLATFORM": "offscreen"}
    r = subprocess.run([sys.executable, "-c", SCRIPT, str(elf)], env=env, capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
