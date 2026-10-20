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

from .analog import _header as _fields_header, classic_blocks, describe
from .classic import classic
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
    """Timer instances of `mmcu`, TCA then TCB then TCD, by instance number. A classic AVR has one
    "Timers" entry (kind CLASSIC) holding all of its TCCRn / TCNTn / OCRn / TIMSK / TIFR registers."""
    if classic(mmcu):
        return tuple(Timer("Timers", "CLASSIC", b.regs) for b in classic_blocks(mmcu, "timer", "Timers"))
    found: dict[str, dict[str, tuple[int, int]]] = {}
    for r in registers(mmcu):
        m = re.fullmatch(r"(TC[ABD])(\d+)\.(\w+(?:\.\w+)?)", r.name)
        if m:
            found.setdefault(m[1] + m[2], {})[m[3]] = (r.address, r.size)
    return tuple(Timer(n, n[:3], found[n]) for n in sorted(found, key=lambda n: (n[:3], int(n[3:]))))


Read = Callable[[int, int], int]    # (address, size) -> value

# ATtiny13A Timer/Counter0, DS40002307A Tables 11-2..11-9 and the frequency equations of 11.7-11.9
_T13_CLOCK = {0: "stopped", 1: "clk/1", 2: "clk/8", 3: "clk/64", 4: "clk/256", 5: "clk/1024",
              6: "external T0 (PB2), falling edge", 7: "external T0 (PB2), rising edge"}
_T13_DIV = {1: 1, 2: 8, 3: 64, 4: 256, 5: 1024}
_T13_WGM = {0: ("Normal", "0xFF"), 1: ("Phase correct PWM", "0xFF"), 2: ("CTC", "OCR0A"),
            3: ("Fast PWM", "0xFF"), 4: ("Reserved", ""), 5: ("Phase correct PWM", "OCR0A"),
            6: ("Reserved", ""), 7: ("Fast PWM", "OCR0A")}
_T13_COM = {  # (kind, COM[1:0]) -> text; kind: "plain" (Normal/CTC), "fast", "phase"
    ("plain", 0): "disconnected", ("plain", 1): "toggle on compare match",
    ("plain", 2): "clear on compare match", ("plain", 3): "set on compare match",
    ("fast", 0): "disconnected", ("phase", 0): "disconnected",
    ("fast", 2): "clear on compare match, set at TOP", ("fast", 3): "set on compare match, clear at TOP",
    ("phase", 2): "clear on compare match up-counting, set down-counting",
    ("phase", 3): "set on compare match up-counting, clear down-counting",
}


def _tiny13_timer0(r: Callable[[str], int], clk_hz: int) -> list[tuple[str, str]]:
    """Derived Timer0 rows: clock source, mode and TOP, the output pins' behaviour and the counter
    cycle. Data-sheet arithmetic on the register values, not a simulation of the waveform."""
    a, b = r("TCCR0A"), r("TCCR0B")
    cs, wgm = b & 7, (a & 3) | (b >> 3 & 1) << 2
    name, top = _T13_WGM[wgm]
    rows = [("Clock source", _T13_CLOCK[cs])]
    div = _T13_DIV.get(cs)
    tick = clk_hz / div if div and clk_hz else 0
    if tick:
        rows.append(("Counter tick", f"{tick:,.0f} Hz ({1e6 / tick:.4g} us)"))
    rows.append(("Mode", f"{wgm}: {name}" + (f", TOP = {top}" if top else "")))
    kind = {0: "plain", 2: "plain", 3: "fast", 7: "fast", 1: "phase", 5: "phase"}.get(wgm)
    for out, pin, com in (("OC0A", "PB0", a >> 6 & 3), ("OC0B", "PB1", a >> 4 & 3)):
        if kind is None:
            text = "reserved mode"
        elif com == 1 and kind != "plain":      # toggle exists only for OC0A with WGM02 = 1
            text = ("toggle on compare match" if out == "OC0A" and b & 8 else
                    "disconnected" if out == "OC0A" else "reserved")
        else:
            text = _T13_COM.get((kind, com), "reserved")
        rows.append((out, f"{text} ({pin})" + ("" if com == 0 or kind is None else
                                                "  - drives the pin only while its DDR bit is set")))
    if kind is not None:
        top_n = r("OCR0A") if top == "OCR0A" else 0xFF
        cycle = 2 * top_n if kind == "phase" else top_n + 1
        if cycle:
            text = f"{cycle} ticks"
            if tick:
                f = tick / cycle
                text += f" = {cycle / tick * 1e3:.4g} ms ({f:,.0f} Hz)" if f >= 100 else f" = {cycle / tick * 1e3:.4g} ms ({f:.4g} Hz)"
            rows.append(("Counter cycle", text))
    return rows


def decode(mmcu: str, t: Timer, read: Read, clk_hz: int = 0) -> tuple[tuple[str, str], ...]:
    """Labelled (field, text) rows for one timer; empty if the header is unreadable. A classic
    timer block gives one row per register: its value and the header's named bits and fields."""
    if t.kind == "CLASSIC":
        fh = _fields_header(mmcu)
        rows = [(reg, f"0x{(v := read(addr, size)):0{2 * size}X}  "
                      + describe(fh.fields.get(("TIMER", reg), ()) if fh else (), v))
                for reg, (addr, size) in t.regs.items()]
        if mmcu.startswith("attiny13") and {"TCCR0A", "TCCR0B", "OCR0A", "OCR0B"} <= t.regs.keys():
            rows += _tiny13_timer0(lambda n: read(*t.regs[n]), clk_hz)
        return tuple(rows)
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
