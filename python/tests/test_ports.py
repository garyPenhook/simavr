"""PORT pin decoding (no Qt, no firmware) plus the Ports dock against real firmware."""

import os
import subprocess
import sys

from simavr_py.ports import decode_pin, ports


def test_ports_atmega4809():
    p = {x.name: x for x in ports("atmega4809")}
    assert {"PORTA", "PORTF"} <= set(p)
    assert p["PORTA"].dir == 0x0400 and p["PORTA"].out == 0x0404 and p["PORTA"].inp == 0x0408
    assert p["PORTA"].pinctrl == tuple(0x0410 + i for i in range(8))


def test_unknown_device_has_no_ports():
    assert ports("atmega328") == () and ports("") == ()


def test_decode_pin():
    s = decode_pin("PORTB", 3, 0b1000, 0b0000, 0b1000, 0x80 | 0x08 | 0x3)
    assert (s.name, s.output, s.out, s.level) == ("PB3", True, False, True)
    assert s.inverted and s.pullup and s.sense == "falling"
    s = decode_pin("PORTA", 0, 0, 0, 0, 0x07)
    assert not s.output and not s.inverted and not s.pullup and s.sense == "reserved"


SCRIPT = """
import sys
from PySide6.QtWidgets import QApplication
from simavr_py.gui.window import MainWindow

app = QApplication([])
win = MainWindow("atmega4809", 3333333, speed=0)
win.load(sys.argv[1])
win.run(); win.pause(); assert win._sim.wait_stopped(5)
win.refresh()
t = win.portpane._table
assert t.rowCount() == 8 * len(win.portpane._ports) > 0
assert [t.item(0, c).text() for c in (0, 1, 2)] == ["PA0", "out", "1"], [t.item(0, c).text() for c in range(7)]
assert t.item(1, 1).text() == "in" and t.item(1, 5).text() == "yes"      # PA1: pull-up set
win.close()
"""


def test_port_pane(tmp_path):
    src = tmp_path / "p.c"
    src.write_text("#include <avr/io.h>\nint main(void){PORTA.DIRSET=1;PORTA.OUTSET=1;"
                   "PORTA.PIN1CTRL=PORT_PULLUPEN_bm;for(;;);}\n")
    elf = tmp_path / "p.elf"
    r = subprocess.run(["avr-gcc", "-mmcu=atmega4809", "-Os", "-o", elf, src],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    env = {**os.environ, "QT_QPA_PLATFORM": "offscreen"}
    r = subprocess.run([sys.executable, "-c", SCRIPT, str(elf)], env=env, capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
