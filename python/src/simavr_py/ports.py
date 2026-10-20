"""PORT pin state decoding for modern AVRs (Qt-free).

`ports(mmcu)` lists the PORTx instances of a device; `decode_pin(dir_, out, in_, ctrl, n)` turns
the raw PORTx.DIR/OUT/IN bytes and one PINnCTRL byte into a `PinState`. Bit layout of PINnCTRL
(DS40002205A 16.5.11): INVEN bit 7, PULLUPEN bit 3, ISC[2:0] bits 2:0.
"""

from __future__ import annotations

from dataclasses import dataclass
from functools import lru_cache

from .classic import classic, pin_bits
from .registers import registers

INVEN = 0x80
PULLUPEN = 0x08
ISC_NAMES = {0: "none", 1: "both", 2: "rising", 3: "falling", 4: "off", 5: "low"}


@dataclass(frozen=True)
class PortRegs:
    name: str           # "PORTA"
    dir: int
    out: int
    inp: int
    pinctrl: tuple[int, ...]    # PIN0CTRL..PIN7CTRL addresses (none on a classic AVR)
    bits: tuple[int, ...] = tuple(range(8))     # pins the device has (ATtiny13A PORTB: 0-5)
    pud: tuple[int, int] | None = None          # classic: (address, mask) of the pull-up disable bit


@dataclass(frozen=True)
class PinState:
    name: str           # "PA3"
    output: bool        # DIR bit
    out: bool           # OUT bit
    level: bool         # IN bit
    inverted: bool
    pullup: bool
    sense: str


def _classic_ports(mmcu: str) -> tuple[PortRegs, ...]:
    """PORTx/DDRx/PINx of a classic AVR, with only the pins the header defines for the port."""
    h = classic(mmcu)
    by = {n: a for n, a, _ in h.regs} if h else {}
    pud = next(((by[r], 1 << b["PUD"]) for r, b in h.bits.items() if "PUD" in b and r in by), None) if h else None
    return tuple(PortRegs(f"PORT{x}", by[f"DDR{x}"], by[f"PORT{x}"], by[f"PIN{x}"], (),
                          pin_bits(h, f"PORT{x}") or tuple(range(8)), pud)
                 for x in sorted({n[-1] for n in by if n.startswith("PORT") and len(n) == 5})
                 if f"DDR{x}" in by and f"PIN{x}" in by)


@lru_cache(maxsize=None)
def ports(mmcu: str) -> tuple[PortRegs, ...]:
    if classic(mmcu):
        return _classic_ports(mmcu)
    by = {r.name: r.address for r in registers(mmcu)}
    out = []
    for name in sorted({n.split(".")[0] for n in by if n.startswith("PORT") and len(n.split(".")[0]) == 5}):
        try:
            ctrl = tuple(by[f"{name}.PIN{i}CTRL"] for i in range(8))
            out.append(PortRegs(name, by[f"{name}.DIR"], by[f"{name}.OUT"], by[f"{name}.IN"], ctrl))
        except KeyError:
            continue
    return tuple(out)


def decode_pin(port: str, n: int, dir_: int, out: int, inp: int, ctrl: int) -> PinState:
    return PinState(f"P{port[-1]}{n}", bool(dir_ >> n & 1), bool(out >> n & 1), bool(inp >> n & 1),
                    bool(ctrl & INVEN), bool(ctrl & PULLUPEN), ISC_NAMES.get(ctrl & 7, "reserved"))


def decode_pin_classic(port: str, n: int, ddr: int, out: int, inp: int, pud: bool) -> PinState:
    """A classic pin: DDRx bit = direction, PORTx bit = output latch, or pull-up when the pin is an
    input and PUD (pull-up disable) is clear. No invert or sense control per pin."""
    output = bool(ddr >> n & 1)
    latch = bool(out >> n & 1)
    return PinState(f"P{port[-1]}{n}", output, latch, bool(inp >> n & 1), False,
                    not output and latch and not pud, "")
