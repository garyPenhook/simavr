"""Named IO registers of a modern AVR (Qt-free), read from the device's avr-libc header.

`registers("atmega4809")` flattens every peripheral instance (`#define TCA0 (*(TCA_t *) 0x0A00)`)
through its struct/union typedefs into `Register(name, address, size)` rows such as
`TCA0.SINGLE.CTRLA`. Names and addresses are exactly what firmware sees through that header
(the same source the cores use). Reserved padding is skipped; both arms of a union are listed.
Modern tinyAVR / megaAVR-0 style headers (struct-per-peripheral) are flattened here; classic
headers (`_SFR_IO8`) come from `classic`. A missing or unparsable header gives an empty list, never
an error.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from functools import lru_cache

from .classic import classic, group_of
from .vectors import _search_dirs, header_name

_TYPEDEF = re.compile(r"typedef\s+(struct|union)\s+\w+\s*\{(.*?)\}\s*(\w+)\s*;", re.S)
_INSTANCE = re.compile(r"^#\s*define\s+(\w+)\s+\(\*\(\s*(\w+)\s*\*\)\s*(0x[0-9A-Fa-f]+|\d+)\s*\)", re.M)
_MEMBER = re.compile(r"^\s*(\w+)\s+(\w+)\s*(?:\[\s*(\d+)\s*\])?\s*;")
_WORD = re.compile(r"^\s*_WORDREGISTER\(\s*(\w+)\s*\)\s*;")
_COMMENT = re.compile(r"/\*.*?\*/", re.S)


@dataclass(frozen=True)
class Register:
    name: str       # "TCA0.SINGLE.CTRLA"
    address: int    # data-space address
    size: int       # bytes (1, or 2 for 16-bit registers)


def _parse_types(text: str) -> dict[str, tuple[str, list[tuple]]]:
    """{type: (kind, members)}; member = ("reg", name, n_bytes, count) or ("sub", name, type)."""
    types = {}
    for kind, body, name in _TYPEDEF.findall(_COMMENT.sub("", text)):
        members = []
        for line in body.splitlines():
            if m := _WORD.match(line):
                members.append(("reg", m[1], 2, 1))
            elif m := _MEMBER.match(line):
                ty, mem, count = m[1], m[2], int(m[3] or 1)
                if ty == "register8_t":
                    members.append(("reg", mem, 1, count))
                elif ty == "register16_t":
                    members.append(("reg", mem, 2, count))
                else:
                    members.append(("sub", mem, ty, count))
        types[name] = (kind, members)
    return types


def _size(types, ty: str) -> int:
    kind, members = types[ty]
    sizes = [(m[2] * m[3]) if m[0] == "reg" else _size(types, m[2]) * m[3] for m in members]
    return (max(sizes, default=0) if kind == "union" else sum(sizes))


def _flatten(types, ty: str, prefix: str, base: int, out: list[Register]):
    kind, members = types[ty]
    off = 0
    for m in members:
        if m[0] == "reg":
            _, name, width, count = m
            if not name.startswith("reserved"):
                for i in range(count):
                    out.append(Register(f"{prefix}.{name}" + (str(i) if count > 1 else ""),
                                        base + off + i * width, width))
            step = width * count
        else:
            _, name, sub, count = m
            step = _size(types, sub)
            for i in range(count):
                _flatten(types, sub, f"{prefix}.{name}" + (str(i) if count > 1 else ""),
                         base + off + i * step, out)
            step *= count
        if kind == "struct":
            off += step


@lru_cache(maxsize=None)
def registers(mmcu: str) -> tuple[Register, ...]:
    """All named registers of `mmcu`, sorted by address; empty when unknown."""
    name = header_name(mmcu or "")
    if name is None:
        return ()
    for d in _search_dirs():
        try:
            text = (d / name).read_text(errors="replace")
        except OSError:
            continue
        types = _parse_types(text)
        out: list[Register] = []
        for inst, ty, addr in _INSTANCE.findall(text):
            if ty in types:
                _flatten(types, ty, inst, int(addr, 0), out)
        if out:
            return tuple(sorted(out, key=lambda r: (r.address, r.name)))
    h = classic(mmcu)
    return tuple(Register(n, a, s) for n, a, s in h.regs) if h else ()


def _classic_pairs(by_name):
    """(flag register, mask register) of a classic device, the mask found by name: xIFR pairs with
    xIMSK or xICR (GIFR/GIMSK or GICR, EIFR/EIMSK, PCIFR/PCICR, TIFRn/TIMSKn, ETIFR/ETIMSK).
    Flags that share a register with their enable (ADIF/ADIE, WDTIF/WDTIE) are `interrupt_flag_bits`."""
    for name in sorted(by_name):
        if m := re.fullmatch(r"(\w*)IFR(\d*)", name):
            mask = next((n for n in (f"{m[1]}IMSK{m[2]}", f"{m[1]}ICR{m[2]}") if n in by_name), None)
            yield name, mask


def interrupt_flag_pairs(mmcu: str) -> tuple[tuple[Register, Register | None], ...]:
    """(INTFLAGS, INTCTRL-or-None) per peripheral instance that has an INTFLAGS register (a classic
    device: its TIFRn/GIFR-style flag registers with their mask registers).
    The VPORT aliases are left out (they mirror the PORT ones), and of a union's overlapping
    arms (TCA0.SINGLE / TCA0.SPLIT) only the first by name is kept."""
    by_name = {r.name: r for r in registers(mmcu)}
    if classic(mmcu):
        return tuple((by_name[f], by_name.get(m)) for f, m in _classic_pairs(by_name))
    pairs, seen = [], set()
    for name in sorted(by_name):
        reg = by_name[name]
        if name.endswith(".INTFLAGS") and not name.startswith("VPORT") and reg.address not in seen:
            seen.add(reg.address)
            pairs.append((reg, by_name.get(name[:-len("INTFLAGS")] + "INTCTRL")))
    return tuple(pairs)


def interrupt_flag_bits(mmcu: str) -> tuple[tuple[str, Register, int, int], ...]:
    """(label, register, flag mask, enable mask) for the flag/enable bit pairs that share one
    register on a classic device: ADIF/ADIE in ADCSRA, WDTIF/WDTIE in WDTCR, ACI/ACIE in ACSR.
    The enable bit is named FOOIE (flag FOOIF) or FOOIE with the flag FOOI. Empty for a modern
    device, whose flags are all in INTFLAGS registers (`interrupt_flag_pairs`)."""
    h = classic(mmcu)
    if h is None:
        return ()
    by_name = {r.name: r for r in registers(mmcu)}
    out = []
    for reg, bits in h.bits.items():
        if reg not in by_name or group_of(reg) in ("port", "cpu"):
            continue
        for name, bit in bits.items():
            m = re.fullmatch(r"(\w+?)IE(\d*)", name)
            if not m:
                continue
            flag = next((f for f in (f"{m[1]}IF{m[2]}", f"{m[1]}I{m[2]}") if f in bits and f != name), None)
            if flag:
                out.append((flag, by_name[reg], 1 << bits[flag], 1 << bit))
    return tuple(sorted(out, key=lambda r: (r[1].address, r[2])))
