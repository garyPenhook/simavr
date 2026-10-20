"""USART / SPI / TWI register state of a modern AVR (Qt-free), decoded from the device's avr-libc header.

Same decoding as `analog` (rows of register value + header-defined fields, no table here), for the
serial blocks: `comms(mmcu)` lists the instances (`USART0`, `SPI0`, `TWI0` ...) and `decode(...)`
turns live register bytes into rows. One derived row for each USART, the data-sheet baud
arithmetic (DS40002173C Table 23-1): fBAUD = 64 * fCLK_PER / (S * BAUD) with S = 16 (asynchronous
normal), 8 (asynchronous double-speed, RXMODE=CLK2X) or 2 (synchronous, BAUD[15:6] is used with
f = fCLK_PER / (S * BAUD[15:6])). It assumes fCLK_PER equals the simulated CPU clock passed as
`clk_per` (no CLKCTRL prescaler is read). A register view: it never changes simulator state.
"""

from __future__ import annotations

import re
from functools import lru_cache

from .analog import Block, Read, _header, describe
from .registers import registers

KINDS = ("USART", "SPI", "TWI")


@lru_cache(maxsize=None)
def comms(mmcu: str) -> tuple[Block, ...]:
    """Serial peripheral instances of `mmcu`: USART, SPI, TWI, each by instance number."""
    found: dict[str, dict[str, tuple[int, int]]] = {}
    for r in registers(mmcu):
        m = re.fullmatch(r"((?:USART|SPI|TWI)\d+)\.(\w+)", r.name)
        if m:
            found.setdefault(m[1], {})[m[2]] = (r.address, r.size)
    return tuple(Block(n, n.rstrip("0123456789"), dict(sorted(found[n].items(), key=lambda kv: kv[1])))
                 for n in sorted(found, key=lambda n: (KINDS.index(n.rstrip("0123456789")), len(n), n)))


def usart_baud(baud: int, mode: str, rxmode: str, clk_per: int) -> str:
    """Baud rate the BAUD register gives, or "" when it can't be computed."""
    if mode == "SYNCHRONOUS":
        div = (baud >> 6) * 2
    elif mode in ("ASYNCHRONOUS", "IRCOM"):
        div = baud * (8 if rxmode == "CLK2X" else 16) / 64
    else:
        return ""
    if div <= 0 or clk_per <= 0:
        return ""
    return f"BAUD = {baud} -> {clk_per / div:.6g} bit/s at CLK_PER = {clk_per:g} Hz"


def decode(mmcu: str, block: Block, read: Read, clk_per: int = 0) -> tuple[tuple[str, str, str], ...]:
    """(register, "0x.." value, fields) per register of `block`, then derived rows (value "").
    Empty if the header is unreadable."""
    h = _header(mmcu)
    if h is None:
        return ()
    out, vals = [], {}
    for reg, (addr, size) in block.regs.items():
        value = vals[reg] = read(addr, size)
        out.append((reg, f"0x{value:0{2 * size}X}", describe(h.fields.get((block.kind, reg), ()), value)))
    if block.kind == "USART" and "BAUD" in vals:
        def opt(reg: str, name: str) -> str:
            f = next((f for f in h.fields.get((block.kind, reg), ()) if f.name == name), None)
            return "" if f is None else f.options.get(
                (vals.get(reg, 0) & f.mask) >> ((f.mask & -f.mask).bit_length() - 1), "")
        text = usart_baud(vals["BAUD"], opt("CTRLC", "CMODE"), opt("CTRLB", "RXMODE"), clk_per)
        if text:
            out.append(("Baud rate", "", text))
    return tuple(out)
