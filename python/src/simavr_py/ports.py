"""PORT pin state decoding for modern AVRs (Qt-free).

`ports(mmcu)` lists the PORTx instances of a device; `decode_pin(dir_, out, in_, ctrl, n)` turns
the raw PORTx.DIR/OUT/IN bytes and one PINnCTRL byte into a `PinState`. Bit layout of PINnCTRL
(DS40002205A 16.5.11): INVEN bit 7, PULLUPEN bit 3, ISC[2:0] bits 2:0.
"""

from __future__ import annotations

from dataclasses import dataclass

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
    pinctrl: tuple[int, ...]    # PIN0CTRL..PIN7CTRL addresses


@dataclass(frozen=True)
class PinState:
    name: str           # "PA3"
    output: bool        # DIR bit
    out: bool           # OUT bit
    level: bool         # IN bit
    inverted: bool
    pullup: bool
    sense: str


def ports(mmcu: str) -> tuple[PortRegs, ...]:
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
