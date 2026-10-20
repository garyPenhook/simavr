"""PORTMUX routing fields of a modern AVR (Qt-free), read from the device's avr-libc header.

`routes("atmega4809")` lists every routing field of the device (`USART0`, `TCB2`, ...) with the
register and bits that select it and the options this device offers, e.g. USART0: 0 DEFAULT
"USART0 on PA[3:0]", 1 ALT1 "USART0 on PA[7:4]", 3 NONE. The options and their text come from
the header's `PORTMUX_<field>_<option>_gc` enums, so a smaller package lists fewer options. The
megaAVR-0 and tinyAVR 2-series headers say which pins an option uses; the tinyAVR 0/1-series
ones only say "Default pins" / "Alternate pins" (the pin tables are in the data sheet). The
simulator keeps PORTMUX as a register store: it doesn't change which pins peripherals reach.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from functools import lru_cache

from .registers import registers
from .vectors import _search_dirs, header_name

_ENUM = re.compile(r"typedef\s+enum\s+PORTMUX_(\w+?)_enum\s*\{(.*?)\}\s*PORTMUX_\w+_t\s*;", re.S)
_OPTION = re.compile(r"PORTMUX_\w+?_(\w+?)_gc\s*=\s*\(\s*(0x[0-9A-Fa-f]+|\d+)\s*<<\s*(\d+)\s*\)"
                     r"\s*,?\s*(?:/\*\s*(.*?)\s*\*/)?", re.S)
_REG = re.compile(r"/\*\s*PORTMUX\.(\w+)\s+bit masks and bit positions\s*\*/")
_MASK = re.compile(r"^\s*#\s*define\s+PORTMUX_(\w+?)_(?:bm|gm)\s+(0x[0-9A-Fa-f]+|\d+)", re.M)


@dataclass(frozen=True)
class RouteField:
    name: str                   # "USART0"
    register: str               # "PORTMUX.USARTROUTEA"
    address: int                # data-space address of that register
    mask: int                   # bits of the register this field uses
    shift: int
    options: tuple[tuple[int, str, str], ...]   # (value, name, text), sorted by value

    def decode(self, register_value: int) -> tuple[int, str, str]:
        """(field value, option name, text); an undefined value reads as "reserved"."""
        v = (register_value & self.mask) >> self.shift
        for value, name, text in self.options:
            if value == v:
                return v, name, text
        return v, "reserved", ""


def _fields(text: str) -> list[tuple[str, int, int, str, list[tuple[int, str, str]]]]:
    """[(field, mask, shift, register, options)] in header order."""
    # Each "PORTMUX.<REG> bit masks" comment is followed by the mask defines of its fields.
    where: dict[str, tuple[str, int]] = {}      # field -> (register, mask)
    events = sorted([(m.start(), "reg", m[1], 0) for m in _REG.finditer(text)] +
                    [(m.start(), "mask", m[1], int(m[2], 0)) for m in _MASK.finditer(text)])
    reg = None
    for _, kind, name, value in events:
        if kind == "reg":
            reg = name
        elif reg is not None:
            where.setdefault(name, (reg, value))
    out = []
    for em in _ENUM.finditer(text):
        field = em[1]
        opts = [(int(o[2], 0), o[1], (o[4] or "").strip(), int(o[3]))
                for o in _OPTION.finditer(em[2])]
        if not opts or field not in where:
            continue
        shift = opts[0][3]
        out.append((field, where[field][1], shift, where[field][0],
                    [(v, n, t) for v, n, t, _ in opts]))
    # tinyAVR 0/1-series PORTMUX.CTRLA EVOUTn bits have no enum in the header: a set bit
    # enables the event output (DS40002205A 15.3.1; same text in DS40002204A / DS40002287A),
    # unlike the *ROUTEA headers where EVOUT picks a pin.
    have = {f for f, *_ in out}
    for field, (reg, mask) in where.items():
        if re.fullmatch(r"EVOUT\d", field) and field not in have and mask & (mask - 1) == 0:
            out.append((field, mask, mask.bit_length() - 1, reg,
                        [(0, "OFF", "Event output disabled"), (1, "ON", "Event output enabled")]))
    return out


@lru_cache(maxsize=None)
def routes(mmcu: str) -> tuple[RouteField, ...]:
    """All PORTMUX routing fields of `mmcu`, ordered by register address then bit position;
    empty when the device or its header is unknown."""
    name = header_name(mmcu or "")
    if name is None:
        return ()
    addr = {r.name: r.address for r in registers(mmcu)}
    for d in _search_dirs():
        try:
            text = (d / name).read_text(errors="replace")
        except OSError:
            continue
        found = []
        for field, mask, shift, reg, opts in _fields(text):
            full = f"PORTMUX.{reg}"
            if full in addr:
                found.append(RouteField(field, full, addr[full], mask, shift,
                                        tuple(sorted(opts))))
        if found:
            return tuple(sorted(found, key=lambda f: (f.address, f.shift, f.name)))
    return ()
