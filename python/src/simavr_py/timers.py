"""TCA / TCB / TCD state of a modern AVR (Qt-free), decoded from the device's avr-libc header.

`timers(mmcu)` lists the timer instances (`TCA0`, `TCB0`, `TCD0`, ...) with the data-space
address of each register; `decode(timer, read)` turns the live register bytes into labelled
rows (enable, clock, mode, counter, period, compares, output enables, interrupt enables and
flags). Mode and clock names are the header's `TCx_<FIELD>_<option>_gc` enum names and the
field masks are its `_gm`/`_bm` defines, so each family's own layout (e.g. the 2-series TCB
3-bit CLKSEL) is followed. Header gives no WGMODE enum for TCD, so that is shown as a number.
Read-only: it reflects registers, not the waveform on the pins (use the Timeline for that).
"""

from __future__ import annotations

import re
from collections.abc import Callable
from dataclasses import dataclass
from functools import lru_cache

from .registers import registers
from .vectors import _search_dirs, header_name

_ENUM_OPT = re.compile(r"(TC[ABD]_\w+?)_gc\s*=\s*\(\s*(0x[0-9A-Fa-f]+|\d+)\s*<<\s*(\d+)\s*\)")
_DEFINE = re.compile(r"^\s*#\s*define\s+(TC[ABD]_\w+?)_(?:gm|bm)\s+(?:(0x[0-9A-Fa-f]+|\d+)|\(1<<(\d+)\))", re.M)


@dataclass(frozen=True)
class Timer:
    name: str                   # "TCA0"
    kind: str                   # "TCA" | "TCB" | "TCD"
    regs: dict[str, tuple[int, int]]    # "CTRLA" / "SINGLE.CNT" -> (address, size)

    def __hash__(self):
        return hash((self.name, self.kind))


class _Header:
    def __init__(self, text: str):
        self.masks: dict[str, int] = {}
        for m in _DEFINE.finditer(text):
            self.masks.setdefault(m[1], int(m[2], 0) if m[2] else 1 << int(m[3]))
        self.enums = [(m[1], int(m[2], 0) << int(m[3])) for m in _ENUM_OPT.finditer(text)]

    def field(self, prefix: str, reg_value: int) -> tuple[int, str]:
        """(value, option name or number) of group field `prefix`, e.g. "TCA_SINGLE_CLKSEL"."""
        mask = self.masks.get(prefix, 0)
        if not mask:
            return 0, "?"
        shift = (mask & -mask).bit_length() - 1
        v = (reg_value & mask) >> shift
        for full, value in self.enums:
            if value == v << shift and full.startswith(prefix + "_"):
                return v, full[len(prefix) + 1:]
        return v, str(v)

    def bit(self, name: str, reg_value: int) -> bool:
        return bool(reg_value & self.masks.get(name, 0))


@lru_cache(maxsize=None)
def _header(mmcu: str) -> _Header | None:
    name = header_name(mmcu or "")
    for d in _search_dirs():
        try:
            return _Header((d / name).read_text(errors="replace")) if name else None
        except OSError:
            continue
    return None


@lru_cache(maxsize=None)
def timers(mmcu: str) -> tuple[Timer, ...]:
    """Timer instances of `mmcu`, TCA then TCB then TCD, by instance number."""
    found: dict[str, dict[str, tuple[int, int]]] = {}
    for r in registers(mmcu):
        m = re.fullmatch(r"(TC[ABD])(\d+)\.(\w+(?:\.\w+)?)", r.name)
        if m:
            found.setdefault(m[1] + m[2], {})[m[3]] = (r.address, r.size)
    return tuple(Timer(n, n[:3], found[n]) for n in sorted(found, key=lambda n: (n[:3], int(n[3:]))))


Read = Callable[[int, int], int]    # (address, size) -> value


def decode(mmcu: str, t: Timer, read: Read) -> tuple[tuple[str, str], ...]:
    """Labelled (field, text) rows for one timer; empty if the header is unreadable."""
    h = _header(mmcu)
    if h is None:
        return ()

    def r(name: str) -> int:
        a = t.regs.get(name)
        return read(*a) if a else 0

    rows: list[tuple[str, str]] = []
    add = lambda k, v: rows.append((k, str(v)))
    yn = lambda b: "yes" if b else "no"
    on = lambda names, value, pre: ",".join(n for n in names if h.bit(f"{pre}_{n}", value)) or "-"

    if t.kind == "TCA":
        ctrld = r("SINGLE.CTRLD")
        split = h.bit("TCA_SINGLE_SPLITM", ctrld)
        pre = "TCA_SPLIT" if split else "TCA_SINGLE"
        a, b = r("SINGLE.CTRLA"), r("SINGLE.CTRLB")
        add("Enabled", yn(h.bit(f"{pre}_ENABLE", a)))
        add("Clock", h.field(f"{pre}_CLKSEL", a)[1])
        if split:
            add("Mode", "SPLIT (two 8-bit)")
            add("Outputs enabled", on([f"{x}CMP{n}EN" for x in "LH" for n in range(3)], b, pre))
            for n in ("CNT", "PER", *(f"CMP{i}" for i in range(3))):
                add(f"L{n} / H{n}", f"{r('SPLIT.L' + n)} / {r('SPLIT.H' + n)}")
            ie, fl = r("SPLIT.INTCTRL"), r("SPLIT.INTFLAGS")
            names = ["LUNF", "HUNF"] + [f"{x}CMP{n}" for x in "LH" for n in range(3)]
        else:
            add("Mode", h.field("TCA_SINGLE_WGMODE", b)[1])
            add("Outputs enabled", on([f"CMP{n}EN" for n in range(3)], b, pre))
            add("Direction", "down" if h.bit("TCA_SINGLE_DIR", r("SINGLE.CTRLESET")) else "up")
            add("CNT", r("SINGLE.CNT"))
            add("PER", r("SINGLE.PER"))
            for n in range(3):
                add(f"CMP{n}", f"{r(f'SINGLE.CMP{n}')}  (buf {r(f'SINGLE.CMP{n}BUF')})")
            ie, fl = r("SINGLE.INTCTRL"), r("SINGLE.INTFLAGS")
            names = ["OVF"] + [f"CMP{n}" for n in range(3)]
        add("Int enabled", on(names, ie, pre))
        add("Int flags", on(names, fl, pre))
    elif t.kind == "TCB":
        a, b = r("CTRLA"), r("CTRLB")
        add("Enabled", yn(h.bit("TCB_ENABLE", a)))
        add("Clock", h.field("TCB_CLKSEL", a)[1])
        add("Mode", h.field("TCB_CNTMODE", b)[1])
        add("Output pin", yn(h.bit("TCB_CCMPEN", b)))
        add("CNT", r("CNT"))
        add("CCMP", r("CCMP"))
        add("Running", yn(h.bit("TCB_RUN", r("STATUS"))))
        add("Int enabled", on(["CAPT", "OVF"], r("INTCTRL"), "TCB"))
        add("Int flags", on(["CAPT", "OVF"], r("INTFLAGS"), "TCB"))
    else:                                           # TCD
        a, b = r("CTRLA"), r("CTRLB")
        add("Enabled", yn(h.bit("TCD_ENABLE", a)))
        add("Clock", h.field("TCD_CLKSEL", a)[1])
        add("Sync prescaler", h.field("TCD_SYNCPRES", a)[1])
        add("Counter prescaler", h.field("TCD_CNTPRES", a)[1])
        add("WGMODE", h.field("TCD_WGMODE", b)[1])
        add("Outputs enabled", on(["CMPAEN", "CMPBEN", "CMPCEN", "CMPDEN"], r("FAULTCTRL"), "TCD"))
        for n in ("CMPASET", "CMPACLR", "CMPBSET", "CMPBCLR"):
            add(n, r(n) & h.masks.get(f"TCD_{n}", 0xFFF))
        st = r("STATUS")
        add("Status", on(["ENRDY", "CMDRDY", "PWMACTA", "PWMACTB"], st, "TCD"))
        add("Int enabled", on(["OVF", "TRIGA", "TRIGB"], r("INTCTRL"), "TCD"))
        add("Int flags", on(["OVF", "TRIGA", "TRIGB"], r("INTFLAGS"), "TCD"))
    return tuple(rows)
