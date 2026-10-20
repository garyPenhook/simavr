"""Register tables of a classic AVR (Qt-free), read from the device's avr-libc header.

Classic headers (ATtiny13A, ATmega328P ...) define every register as `#define PORTB _SFR_IO8(0x18)`
and list its bit numbers right below it (`#define PORTB0 0`), unlike the struct-per-peripheral
headers of the modern AVRs that `registers`, `analog` and friends parse. This module reads that
flat form: `classic(mmcu)` gives the registers (data-space addresses: `_SFR_IO` adds the 0x20 IO
offset, `_SFR_MEM` is already a data address) and the named bits of each, `group_of(name)` sorts a
register into the dock that shows it, and `fields(...)` groups consecutive bit names that share a
stem (`CS00 CS01 CS02` -> `CS0`, a 3-bit field) so a register decodes like a modern one.
A missing or modern header gives `None`, never an error.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from functools import lru_cache

from .vectors import _search_dirs, header_name

_REG = re.compile(r"^\s*#\s*define\s+(\w+)\s+_SFR_(IO|MEM)(8|16)\s*\(\s*(0x[0-9A-Fa-f]+|\d+)\s*\)", re.M)
_LINE = re.compile(r"^\s*#\s*define\s+(\w+)\s+(?:(\d{1,2})\b|_SFR_(IO|MEM)(8|16)\s*\(\s*(0x[0-9A-Fa-f]+|\d+)\s*\))\s*(?:/\*.*)?$")
_STEM = re.compile(r"^(.*[A-Za-z0-9])(\d)$")
_BIT_N = re.compile(r"_\d+$")           # OCR0A_0: one bit of a data register

IO_OFFSET = 0x20

# register name -> dock; the first matching pattern wins, everything else that is not a port,
# CPU register or the stack pointer lands in "system"
_GROUPS = (
    ("port", re.compile(r"^(PORT|DDR|PIN)[A-Z]$")),
    ("cpu", re.compile(r"^(SREG|SP[HL]?|RAMP[A-Z]|EIND|GPIOR\d)$")),
    ("timer", re.compile(r"^(TCCR|TCNT|OCR|ICR|TIMSK|TIFR|GTCCR|ASSR|TCCR\d)")),
    ("analog", re.compile(r"^(ADC|ADMUX|ACSR|DIDR|ACMUX)")),
    ("serial", re.compile(r"^(UCSR|UBRR|UDR|SPCR|SPSR|SPDR|TW[A-Z]|USI)")),
    ("clock", re.compile(r"^(CLKPR|OSCCAL)$")),
)


@dataclass(frozen=True)
class ClassicHeader:
    regs: tuple[tuple[str, int, int], ...]          # (name, data address, bytes), by address
    bits: dict[str, dict[str, int]]                 # register -> {bit name: bit number}


def group_of(name: str) -> str:
    return next((g for g, rx in _GROUPS if rx.match(name)), "system")


@lru_cache(maxsize=None)
def classic(mmcu: str) -> ClassicHeader | None:
    name = header_name(mmcu or "")
    if name is None:
        return None
    for d in _search_dirs():
        try:
            text = (d / name).read_text(errors="replace")
        except OSError:
            continue
        if "register8_t" in text or not _REG.search(text):      # modern headers carry both forms
            return None
        return _parse(text)
    return None


def _parse(text: str) -> ClassicHeader:
    regs: dict[str, tuple[int, int]] = {}
    bits: dict[str, dict[str, int]] = {}
    cur = None
    for line in text.splitlines():
        if "_vect" in line:
            break                                   # interrupt vectors end the register section
        m = _LINE.match(line)
        if not m:
            continue
        if m[3]:                                    # a register
            addr = int(m[5], 0) + (IO_OFFSET if m[3] == "IO" else 0)
            regs.setdefault(m[1], (addr, int(m[4]) // 8))
            cur = m[1]
        elif cur is not None and not _BIT_N.search(m[1]) and int(m[2]) < 8 * regs[cur][1]:
            bits.setdefault(cur, {}).setdefault(m[1], int(m[2]))      # not a size or count define
    # ADC/ADCW, EEAR/EEARL: keep the longer name of two registers at one address and size
    for name in [n for n in regs if any(o != n and o.startswith(n) and regs[o] == regs[n] for o in regs)]:
        del regs[name]
    ordered = tuple(sorted(((n, a, s) for n, (a, s) in regs.items()), key=lambda r: (r[1], r[0])))
    return ClassicHeader(ordered, {n: b for n, b in bits.items() if n in regs})


def fields(h: ClassicHeader, reg: str) -> tuple[tuple[str, int], ...]:
    """(name, mask) per field of `reg`, low bit first. Bits sharing a stem and adjacent numbers
    form one field; a group spanning the whole register or repeating the register name (PORTB0..5
    in PORTB) is just data and gets no field."""
    stems: dict[str, list[tuple[str, int]]] = {}
    out: list[tuple[str, int]] = []
    for name, bit in h.bits.get(reg, {}).items():
        m = _STEM.match(name)
        if m:
            stems.setdefault(m[1], []).append((name, bit))
        else:
            out.append((name, 1 << bit))
    for stem, members in stems.items():
        nums = sorted(b for _, b in members)
        if len(members) == 1:
            out.append((members[0][0], 1 << nums[0]))
        elif nums == list(range(nums[0], nums[-1] + 1)) and len(members) < 7 and stem != reg:
            out.append((stem, sum(1 << b for b in nums)))
        elif stem != reg and len(members) < 7:      # gaps: keep the bits apart
            out += [(n, 1 << b) for n, b in members]
    return tuple(sorted(out, key=lambda f: f[1] & -f[1]))


def pin_bits(h: ClassicHeader, port: str) -> tuple[int, ...]:
    """Bit numbers PORTx actually has on the device (ATtiny13A PORTB: 0-5)."""
    return tuple(sorted(set(h.bits.get(port, {}).values())))
