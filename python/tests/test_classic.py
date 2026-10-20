"""Classic-AVR (ATtiny13A) support: flat `_SFR_IO8` header tables and the docks that use them.
Facts checked against DS40002307A: Port B is PB5:PB0, MCUCR.PUD is bit 6, CLKPS[3:0] divides by
2^n up to 256 with 9-15 reserved."""

import os
import subprocess
import sys

import pytest

from simavr_py.analog import analog, decode as decode_analog
from simavr_py.classic import classic, fields, group_of
from simavr_py.clock import clkctrl, decode as decode_clock
from simavr_py.ports import decode_pin_classic, ports
from simavr_py.registers import interrupt_flag_bits, interrupt_flag_pairs, registers
from simavr_py.system import system
from simavr_py.timers import decode as decode_timer, timers

T13 = "attiny13a"


def test_register_table():
    regs = {r.name: r for r in registers(T13)}
    assert regs["PORTB"].address == 0x18 + 0x20 and regs["DDRB"].address == 0x17 + 0x20
    assert regs["TCCR0B"].address == 0x33 + 0x20 and regs["MCUCR"].address == 0x35 + 0x20
    assert regs["ADCW"].size == 2 and "ADC" not in regs and "EEAR" not in regs
    addrs = [r.address for r in registers(T13)]
    assert addrs == sorted(addrs)


def test_modern_header_is_not_classic():
    assert classic("atmega4809") is None and classic("") is None and classic("nonsense") is None


def test_fields_group_adjacent_bits():
    h = classic(T13)
    f = dict(fields(h, "TCCR0B"))
    assert f["CS0"] == 0b111 and f["WGM02"] == 0b1000 and f["FOC0A"] == 0x80
    assert "PORTB" not in dict(fields(h, "PORTB"))              # a port's own bits are data
    assert dict(fields(h, "CLKPR"))["CLKPS"] == 0b1111


def test_groups():
    assert group_of("PORTB") == "port" and group_of("TCCR0A") == "timer"
    assert group_of("ADCSRA") == "analog" and group_of("CLKPR") == "clock"
    assert group_of("MCUCR") == "system" and group_of("SREG") == "cpu"


def test_port_b_has_six_pins_and_pud():
    (p,) = ports(T13)
    assert p.name == "PORTB" and p.bits == (0, 1, 2, 3, 4, 5) and p.pinctrl == ()
    assert p.dir == 0x37 and p.out == 0x38 and p.inp == 0x36
    assert p.pud == (0x35 + 0x20, 0x40)


def test_decode_pin_classic():
    s = decode_pin_classic("PORTB", 3, 0b0000, 0b1000, 0b1000, pud=False)
    assert (s.name, s.output, s.level, s.pullup) == ("PB3", False, True, True)
    assert not decode_pin_classic("PORTB", 3, 0b0000, 0b1000, 0b1000, pud=True).pullup
    assert not decode_pin_classic("PORTB", 3, 0b1000, 0b1000, 0b1000, pud=False).pullup   # output


def test_flag_pairs():
    pairs = {f.name: c and c.name for f, c in interrupt_flag_pairs(T13)}
    assert pairs == {"GIFR": "GIMSK", "TIFR0": "TIMSK0"}


def test_blocks_and_decode():
    (adc,) = analog(T13)
    assert set(adc.regs) >= {"ADCSRA", "ADMUX", "ACSR", "ADCW"}
    rows = {r: f for r, _, f in decode_analog(T13, adc, lambda a, s: 0x83)}
    assert "ADEN" in rows["ADCSRA"] and "ADPS=3" in rows["ADCSRA"]
    assert [b.name for b in system(T13)] == ["System"]
    (t,) = timers(T13)
    got = dict(decode_timer(T13, t, lambda a, s: 0x03))
    assert "CS0=3" in got["TCCR0B"]
    (clk,) = clkctrl(T13)
    rows = {r: f for r, _, f in decode_clock(T13, clk, lambda a, s: 0x03, 1200000)}
    assert rows["Clock division"] == "/8"
    rows = {r: f for r, _, f in decode_clock(T13, clk, lambda a, s: 0x09, 1200000)}
    assert rows["Clock division"].startswith("reserved")
    rows = {r: f for r, _, f in decode_clock(T13, clk, lambda a, s: 0x08, 1200000)}
    assert rows["Clock division"] == "/256"
    assert rows["CPU clock"] == "1,200,000 Hz"


def test_flag_bits_sharing_a_register():
    got = {(label, r.name, f, e) for label, r, f, e in interrupt_flag_bits(T13)}
    assert got == {("ADIF", "ADCSRA", 0x10, 0x08), ("ACI", "ACSR", 0x10, 0x08), ("WDTIF", "WDTCR", 0x80, 0x40)}
    assert interrupt_flag_bits("atmega4809") == () and interrupt_flag_bits("nonsense") == ()


def timer_rows(tccr0a, tccr0b, ocr0a=0, hz=1_200_000):
    (t,) = timers(T13)
    vals = {"TCCR0A": tccr0a, "TCCR0B": tccr0b, "OCR0A": ocr0a}
    addr = {a: n for n, (a, _) in t.regs.items()}
    return dict(decode_timer(T13, t, lambda a, s: vals.get(addr[a], 0), hz))


def test_timer0_derived_rows():
    r = timer_rows(0x00, 0x03)                                  # Normal, clk/64
    assert r["Clock source"] == "clk/64" and r["Counter tick"].startswith("18,750 Hz")
    assert r["Mode"] == "0: Normal, TOP = 0xFF" and r["OC0A"].startswith("disconnected")
    assert r["Counter cycle"].startswith("256 ticks")
    r = timer_rows(0b01000010, 0x01, ocr0a=99)                  # CTC, toggle OC0A, clk/1
    assert r["Mode"] == "2: CTC, TOP = OCR0A" and r["OC0A"].startswith("toggle on compare match (PB0)")
    assert r["Counter cycle"].startswith("100 ticks") and "12,000 Hz" in r["Counter cycle"]
    r = timer_rows(0b10100001, 0x02)                            # phase correct, clear on up-count
    assert r["Counter cycle"].startswith("510 ticks") and r["OC0B"].startswith("clear on compare match up-counting")
    r = timer_rows(0b00010011, 0x08 | 0x01)                     # fast PWM TOP=OCRA, COM0B=01 is reserved
    assert r["Mode"] == "7: Fast PWM, TOP = OCR0A" and r["OC0B"].startswith("reserved")
    r = timer_rows(0b00000011, 0x01)                            # fast PWM, both outputs unused
    assert r["OC0A"].startswith("disconnected") and r["OC0B"].startswith("disconnected")
    r = timer_rows(0b00000001, 0x01)                            # phase correct, both unused
    assert r["OC0A"].startswith("disconnected") and r["OC0B"].startswith("disconnected")
    assert timer_rows(0x00, 0x00)["Clock source"] == "stopped"
    assert timer_rows(0x00, 0x06)["Clock source"].startswith("external T0 (PB2), falling")
    assert timer_rows(0x00, 0x08)["Mode"] == "4: Reserved"      # WGM02 alone is mode 4


SCRIPT = """
import sys
from PySide6.QtWidgets import QApplication
from simavr_py.gui.window import MainWindow

app = QApplication([])
win = MainWindow(None, 0, speed=0)
win.load(sys.argv[1])
win.load_board(sys.argv[2])
assert sorted(win.pins._cells) == [f"PB{i}" for i in range(6)], sorted(win.pins._cells)
win.run()
win.pause(); assert win._sim.wait_stopped(5)
win.refresh()
assert win.regs._note.text().startswith("34 of 34"), win.regs._note.text()
t = win.portpane._table
assert t.rowCount() == 6
assert [t.item(0, c).text() for c in (0, 1)] == ["PB0", "out"]
assert [t.item(3, c).text() for c in (0, 1, 2, 5)] == ["PB3", "in", "1", "yes"]     # SW0 pull-up
assert win.timerpane._table.rowCount() > 0 and win.analogpane._table.rowCount() > 0
assert win.clockpane._table.rowCount() > 0 and win.syspane._table.rowCount() > 0
assert win.irqs._flags.text().startswith("flags set") and "n/a" in win.irqs._exec.text()
assert "pending: none" in win.irqs._pending.text()
win.close()
"""


def test_gui_attiny13a(tmp_path):
    here = os.path.join(os.path.dirname(__file__), "..")
    elf = tmp_path / "demo_13a.elf"
    r = subprocess.run(["avr-gcc", "-mmcu=attiny13a", "-Os", "-g", "-I", os.path.join(here, "..", "simavr", "sim", "avr"),
                        "-o", elf, os.path.join(here, "examples", "demo_13a.c")], capture_output=True, text=True)
    if r.returncode:
        pytest.skip("avr-gcc can't target attiny13a: " + r.stderr[:200])
    env = {**os.environ, "QT_QPA_PLATFORM": "offscreen"}
    r = subprocess.run([sys.executable, "-c", SCRIPT, str(elf), os.path.join(here, "boards", "demo_13a.json")],
                       env=env, capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr


# -- engine behaviour behind the docks (firmware-backed) ------------------------------------------

LEVEL_C = """
#include <avr/io.h>
#include <avr/interrupt.h>
volatile unsigned char hits, gifr_seen;
ISR(INT0_vect){ if (++hits >= 5) GIMSK = 0; }
int main(void){
  MCUCR = 0;                          /* ISC0 = low level */
  PORTB = 0x02;
  for (volatile int i = 0; i < 2000; i++) ;      /* the host holds PB1 (INT0) low meanwhile */
  gifr_seen = GIFR;
  GIMSK = _BV(INT0);                  /* enabled while the pin is already low */
  sei();
  for (;;);
}
"""

CLKPR_C = """
#include <avr/io.h>
volatile unsigned char at_reset, after_seq, after_bad, after_reserved;
int main(void){
  at_reset = CLKPR;
  CLKPR = 0x80; CLKPR = 0x01;         /* CLKPCE, then CLKPS = 1: divide by 2 */
  after_seq = CLKPR;
  CLKPR = 0x02;                       /* outside the change window: ignored */
  after_bad = CLKPR;
  CLKPR = 0x80; CLKPR = 0x0F;         /* reserved code: stored, clock unchanged */
  after_reserved = CLKPR;
  for (;;);
}
"""


def build(tmp_path, source, name):
    src, elf = tmp_path / f"{name}.c", tmp_path / f"{name}.elf"
    src.write_text(source)
    r = subprocess.run(["avr-gcc", "-mmcu=attiny13a", "-Os", "-o", elf, src], capture_output=True, text=True)
    if r.returncode:
        pytest.skip("avr-gcc can't target attiny13a: " + r.stderr[:200])
    syms = {}
    for line in subprocess.run(["avr-nm", elf], capture_output=True, text=True).stdout.splitlines():
        addr, _, sym = line.split()
        syms[sym] = int(addr, 16) & 0xFFFF
    return elf, syms


def test_level_interrupt_flag_reads_zero_and_fires_when_enabled_while_low(tmp_path):
    """DS40002307A GIFR.INTF0: "always cleared when INT0 is configured as a level interrupt"; a low
    pin requests the interrupt as soon as it is enabled."""
    from simavr_py import Simulator, Space
    elf, sym = build(tmp_path, LEVEL_C, "level")
    with Simulator(T13, 1200000) as sim:
        sim.load_elf(str(elf))
        sim.drive("PB1", False)
        sim.run_cycles(300000)
        rd = lambda a: sim.read(Space.DATA, a, 1)[0]
        assert rd(sym["gifr_seen"]) == 0 and rd(sym["hits"]) == 5
        gifr = next(r.address for r in registers(T13) if r.name == "GIFR")
        assert rd(gifr) == 0                                    # still 0 while the pin stays low


def test_clkpr_rescales_the_clock(tmp_path):
    """DS40002307A 6.4.2: CKDIV8 resets CLKPS to 3; CLKPCE then CLKPS inside four cycles divides by
    2^CLKPS; anything else is ignored."""
    from simavr_py import Simulator, Space
    elf, sym = build(tmp_path, CLKPR_C, "clkpr")
    with Simulator(T13, 1200000) as sim:
        assert list(sim.read(Space.FUSES, 0, 2)) == [0x6A, 0xFF]
        clkpr = next(r.address for r in registers(T13) if r.name == "CLKPR")
        assert sim.read(Space.DATA, clkpr, 1)[0] == 3 and sim.frequency == 1200000
        sim.load_elf(str(elf))
        sim.run_cycles(5000)
        rd = lambda n: sim.read(Space.DATA, sym[n], 1)[0]
        assert (rd("at_reset"), rd("after_seq"), rd("after_bad"), rd("after_reserved")) == (3, 1, 1, 15)
        assert sim.frequency == 4_800_000                       # 9.6 MHz / 2
        sim.reset()
        assert sim.frequency == 1_200_000 and sim.read(Space.DATA, clkpr, 1)[0] == 3


def test_alias_core_loads_an_elf_built_for_the_other_name(tmp_path):
    from simavr_py import Simulator
    here = os.path.join(os.path.dirname(__file__), "..", "examples", "demo_13a.c")
    elf = tmp_path / "demo.elf"
    r = subprocess.run(["avr-gcc", "-mmcu=attiny13a", "-Os", "-I", os.path.join(os.path.dirname(here), "..", "..", "simavr", "sim", "avr"),
                        "-o", elf, here], capture_output=True, text=True)
    if r.returncode:
        pytest.skip(r.stderr[:200])
    for name in (T13, "attiny13"):                  # both names are one core
        with Simulator(name, 1200000) as sim:
            sim.load_elf(str(elf))
            assert sim.mmcu == name


ROUNDTRIP_C = """
#include <avr/io.h>
int main(void){
  CLKPR = 0x80; CLKPR = 0x08;         /* divide by 256 */
  CLKPR = 0x80; CLKPR = 0x00;         /* and back to /1 */
  for (;;);
}
"""

EDGE_C = """
#include <avr/io.h>
volatile unsigned char flag_in_edge_mode, flag_in_level_mode;
int main(void){
  PORTB = 0x02;
  MCUCR = 0x02;                        /* ISC0 = falling edge */
  for (volatile int i = 0; i < 500; i++) ;      /* the host pulls PB1 low meanwhile: INTF0 latches */
  flag_in_edge_mode = GIFR;
  MCUCR = 0x00;                        /* switch to low level: the flag is cleared */
  flag_in_level_mode = GIFR;
  for (;;);
}
"""


def test_clkpr_frequency_survives_round_trips_exactly(tmp_path):
    from simavr_py import Simulator
    elf, _ = build(tmp_path, ROUNDTRIP_C, "roundtrip")
    with Simulator(T13, 1_000_000) as sim:          # 1 MHz at /8 means an 8 MHz source
        sim.load_elf(str(elf))
        sim.run_cycles(2000)
        assert sim.frequency == 8_000_000


def test_switching_to_level_mode_clears_a_latched_edge_flag(tmp_path):
    from simavr_py import Simulator, Space
    elf, sym = build(tmp_path, EDGE_C, "edge")
    with Simulator(T13, 1200000) as sim:
        sim.load_elf(str(elf))
        sim.run_cycles(20)
        sim.drive("PB1", True)
        sim.run_cycles(20)
        sim.drive("PB1", False)                     # falling edge while ISC0 is falling-edge
        sim.run_cycles(50000)
        rd = lambda n: sim.read(Space.DATA, sym[n], 1)[0]
        assert rd("flag_in_edge_mode") & 0x40 and rd("flag_in_level_mode") == 0
