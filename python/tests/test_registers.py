"""Register table parsed from the avr-libc device headers (no Qt, no firmware)."""

import pytest

from simavr_py.core import cores
from simavr_py.registers import registers

MODERN = [c for c in cores() if registers(c)]


def test_known_addresses_atmega4809():
    d = {r.name: r for r in registers("atmega4809")}
    assert d["CPUINT.STATUS"].address == 0x0111
    assert d["PORTA.OUT"].address == 0x0404 and d["PORTA.OUT"].size == 1
    assert d["USART0.TXDATAL"].address == 0x0802
    assert d["TCA0.SINGLE.CNT"].address == 0x0A20 and d["TCA0.SINGLE.CNT"].size == 2
    assert d["TCA0.SPLIT.CTRLA"].address == 0x0A00       # union arms overlap
    assert not any("reserved" in n for n in d)


def test_known_addresses_attiny3217():
    d = {r.name: r for r in registers("attiny3217")}
    assert d["CPUINT.STATUS"].address == 0x0111
    assert d["PORTA.DIR"].address == 0x0400


def test_every_modern_core_has_a_table():
    assert len(MODERN) == 23                 # 15 tinyAVR 1-series + 8 megaAVR-0
    for c in MODERN:
        regs = registers(c)
        assert len({(r.name) for r in regs}) == len(regs), c      # names unique
        assert [r.address for r in regs] == sorted(r.address for r in regs)


@pytest.mark.parametrize("mmcu", ["atmega328", "attiny85", "", "nonsense"])
def test_unknown_or_classic_is_empty(mmcu):
    assert registers(mmcu) == ()
