"""USART/SPI/TWI decoding for every modern layout (no Qt, no firmware), then the Serial dock
against real firmware."""

import os
import subprocess
import sys

from simavr_py.comms import comms, decode, usart_baud
from simavr_py.registers import registers

CORES = ["atmega4809", "attiny3217", "attiny3224", "attiny1614", "attiny412"]


def rows(mmcu, mem=None, clk=3333333):
    regs = {r.name: r.address for r in registers(mmcu)}
    by_addr = {regs[n]: v for n, v in (mem or {}).items()}
    return {f"{b.name}.{reg}": (value, fields) for b in comms(mmcu)
            for reg, value, fields in decode(mmcu, b, lambda a, s: by_addr.get(a, 0), clk)}


def test_unknown_device_has_none():
    assert comms("atmega328") == () and comms("") == ()


def test_instances_per_device():
    assert [b.name for b in comms("atmega4809")] == ["USART0", "USART1", "USART2", "USART3", "SPI0", "TWI0"]
    assert [b.name for b in comms("attiny3217")] == ["USART0", "SPI0", "TWI0"]


def test_every_core_decodes():
    for m in CORES:
        r = rows(m)
        assert any(k.endswith(".CTRLC") for k in r) and any(k.startswith("SPI0.") for k in r), m
        assert any(k.startswith("TWI0.") for k in r), m


def test_baud_arithmetic():
    # 64 * 3333333 / (16 * 1389) = 9599.2 bit/s; double speed halves S
    assert usart_baud(1389, "ASYNCHRONOUS", "NORMAL", 3333333).startswith("BAUD = 1389 -> 9599.23")
    assert usart_baud(1389, "ASYNCHRONOUS", "CLK2X", 3333333).startswith("BAUD = 1389 -> 19198.5")
    assert " 1e+06 " in usart_baud(1 << 6, "SYNCHRONOUS", "NORMAL", 2000000)     # f / (2 * BAUD[15:6])
    assert usart_baud(0, "ASYNCHRONOUS", "NORMAL", 3333333) == ""
    assert usart_baud(100, "MSPI", "NORMAL", 3333333) == ""
    assert usart_baud(100, "ASYNCHRONOUS", "NORMAL", 0) == ""


def test_decoded_rows():
    r = rows("attiny3217", {"USART0.BAUD": 1389, "USART0.CTRLC": 0x03})
    assert "CMODE=ASYNCHRONOUS" in r["USART0.CTRLC"][1] and "CHSIZE=8BIT" in r["USART0.CTRLC"][1]
    assert r["USART0.BAUD"][0] == "0x056D"
    assert r["USART0.Baud rate"][1].startswith("BAUD = 1389 -> 9599.23")
    r = rows("attiny3217", {"USART0.BAUD": 1389, "USART0.CTRLB": 0x02, "USART0.CTRLC": 0x03})
    assert "-> 19198.5" in r["USART0.Baud rate"][1]
    assert not [k for k in rows("attiny3217") if k.startswith(("SPI0.Baud", "TWI0.Baud"))]


SCRIPT = """
import sys
from PySide6.QtWidgets import QApplication
from simavr_py.gui.window import MainWindow

app = QApplication([])
win = MainWindow("atmega4809", 3333333, speed=0)
win.load(sys.argv[1])
win.run(); win.pause(); assert win._sim.wait_stopped(5)
win.refresh()
p = win.serialpane
assert p._vdd.isHidden()
rows = {(p._table.item(i, 0).text(), p._table.item(i, 1).text()): p._table.item(i, 3).text()
        for i in range(p._table.rowCount())}
assert "RXEN" in rows["USART1", "CTRLB"] and "TXEN" in rows["USART1", "CTRLB"], rows
assert "CHSIZE=8BIT" in rows["USART1", "CTRLC"], rows
assert rows["USART1", "Baud rate"].startswith("BAUD = 1389 -> 9599.23"), rows
assert "ENABLE" in rows["SPI0", "CTRLA"] and "MASTER" in rows["SPI0", "CTRLA"], rows
win.close()
"""


def test_serial_pane(tmp_path):
    src = tmp_path / "t.c"
    src.write_text("#include <avr/io.h>\nint main(void){"
                   "USART1.BAUD=1389;USART1.CTRLC=USART_CHSIZE_8BIT_gc;USART1.CTRLB=USART_RXEN_bm|USART_TXEN_bm;"
                   "SPI0.CTRLA=SPI_MASTER_bm|SPI_ENABLE_bm;for(;;);}\n")
    elf = tmp_path / "t.elf"
    r = subprocess.run(["avr-gcc", "-mmcu=atmega4809", "-Os", "-o", elf, src],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    env = {**os.environ, "QT_QPA_PLATFORM": "offscreen"}
    r = subprocess.run([sys.executable, "-c", SCRIPT, str(elf)], env=env, capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
