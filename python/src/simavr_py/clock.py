"""CLKCTRL register state of a modern AVR (Qt-free), decoded from the device's avr-libc header.

Same decoding as `analog` (register value plus every header-defined field), for the one CLKCTRL
block. Derived rows follow the main clock chain of DS40002205A 10.3 / 10.5.1-10.5.2: CLKSEL picks
CLK_MAIN, and CLK_PER = CLK_MAIN / division when MCLKCTRLB.PEN is set, undivided otherwise
(PDIV is ignored with PEN = 0; reserved PDIV codes have no division). `clk_per` is the simulated
CPU clock the caller passes, which the simulator takes from CLKCTRL itself, so CLK_MAIN is
reported as clk_per x division rather than from a frequency table. A register view: it never
changes simulator state.
"""

from __future__ import annotations

import re
from functools import lru_cache

from .analog import Block, Read, _header, describe
from .registers import registers


@lru_cache(maxsize=None)
def clkctrl(mmcu: str) -> tuple[Block, ...]:
    """The CLKCTRL block of `mmcu` (empty tuple if the device has none)."""
    regs = {m[1]: (r.address, r.size) for r in registers(mmcu)
            if (m := re.fullmatch(r"CLKCTRL\.(\w+)", r.name))}
    return (Block("CLKCTRL", "CLKCTRL", dict(sorted(regs.items(), key=lambda kv: kv[1]))),) if regs else ()


def clock_chain(source: str, pen: bool, pdiv: str, clk_per: int) -> list[tuple[str, str]]:
    """(row, text) for the main clock source, prescaler and resulting CLK_MAIN / CLK_PER."""
    m = re.fullmatch(r"(\d+)X", pdiv)
    div = 1 if not pen else int(m[1]) if m else 0
    out = [("Main clock", source or "?"),
           ("Prescaler", "off (CLK_PER = CLK_MAIN)" if not pen else f"/{div}" if div else f"reserved ({pdiv or '?'})")]
    if clk_per > 0 and div:
        out.append(("CLK_PER", f"{clk_per:g} Hz"))
        out.append(("CLK_MAIN", f"{clk_per * div:g} Hz"))
    return out


def decode(mmcu: str, block: Block, read: Read, clk_per: int = 0) -> tuple[tuple[str, str, str], ...]:
    """(register, "0x.." value, fields) per register of `block`, then derived rows (value "")."""
    h = _header(mmcu)
    if h is None:
        return ()
    out, vals = [], {}
    for reg, (addr, size) in block.regs.items():
        value = vals[reg] = read(addr, size)
        out.append((reg, f"0x{value:0{2 * size}X}", describe(h.fields.get((block.kind, reg), ()), value)))
    if "MCLKCTRLA" in vals and "MCLKCTRLB" in vals:
        def opt(reg: str, name: str) -> str:
            f = next((f for f in h.fields.get((block.kind, reg), ()) if f.name == name), None)
            return "" if f is None else f.options.get(
                (vals[reg] & f.mask) >> ((f.mask & -f.mask).bit_length() - 1), "")
        pen = bool(vals["MCLKCTRLB"] & 1)
        out += [(name, "", text) for name, text in
                clock_chain(opt("MCLKCTRLA", "CLKSEL"), pen, opt("MCLKCTRLB", "PDIV"), clk_per)]
    return tuple(out)
