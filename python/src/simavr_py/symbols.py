"""ELF symbol table reader (Qt-free): flash symbols for the disassembly view.

Only what the GUI needs: little-endian ELF32 (what avr-gcc emits), the `.symtab`
entries in executable sections (value below 0x800000 - avr-gcc places data
symbols at 0x800000 and up) as FUNC or untyped labels. Anything malformed yields an
empty table rather than an error: symbols are decoration, never a reason to
refuse a firmware.
"""

from __future__ import annotations

import struct
from bisect import bisect_right
from dataclasses import dataclass

_DATA_SPACE = 0x800000
_SHT_SYMTAB = 2
_STT_NOTYPE, _STT_FUNC = 0, 2
_SHF_EXECINSTR = 4


@dataclass(frozen=True)
class Symbol:
    addr: int
    name: str
    size: int


class SymbolTable:
    def __init__(self, symbols=()):
        # Sorted by address; at one address prefer a sized FUNC over a bare label.
        best: dict[int, Symbol] = {}
        for s in symbols:
            cur = best.get(s.addr)
            if cur is None or (s.size, s.name) > (cur.size, cur.name):
                best[s.addr] = s
        self._syms = sorted(best.values(), key=lambda s: s.addr)
        self._addrs = [s.addr for s in self._syms]
        self._by_name = {s.name: s for s in self._syms}

    def __len__(self) -> int:
        return len(self._syms)

    def lookup(self, name: str) -> Symbol | None:
        return self._by_name.get(name)

    def find(self, addr: int) -> tuple[Symbol, int] | None:
        """The symbol covering `addr` and the offset into it. A sized symbol
        covers only its extent; a size-0 label runs to the next symbol."""
        i = bisect_right(self._addrs, addr) - 1
        if i < 0:
            return None
        s = self._syms[i]
        off = addr - s.addr
        if s.size and off >= s.size:
            return None
        return s, off

    def describe(self, addr: int) -> str:
        """`name` or `name+0xN` for `addr`, '' when no symbol covers it."""
        hit = self.find(addr)
        if hit is None:
            return ""
        s, off = hit
        return s.name if off == 0 else f"{s.name}+0x{off:x}"


def read_elf_symbols(path: str) -> SymbolTable:
    try:
        with open(path, "rb") as f:
            return SymbolTable(_parse(f.read()))
    except (OSError, struct.error, ValueError, IndexError):
        return SymbolTable()


def _parse(blob: bytes):
    if blob[:4] != b"\x7fELF" or blob[4] != 1 or blob[5] != 1:     # ELF32, LSB
        return
    shoff, = struct.unpack_from("<I", blob, 0x20)
    shentsize, shnum = struct.unpack_from("<HH", blob, 0x2E)
    if not shoff or shentsize < 40:
        return
    secs = [struct.unpack_from("<IIIIIIIIII", blob, shoff + i * shentsize)
            for i in range(shnum)]
    for _name, typ, _fl, _addr, off, size, link, _info, _al, entsize in secs:
        if typ != _SHT_SYMTAB or entsize < 16:
            continue
        stroff, strsize = secs[link][4], secs[link][5]
        strtab = blob[stroff:stroff + strsize]
        for pos in range(off, off + size - 15, entsize):
            nm, value, sz, info, _other, shndx = struct.unpack_from("<IIIBBH", blob, pos)
            kind = info & 0xF
            if (not nm or shndx == 0 or shndx >= min(0xFF00, len(secs))
                    or value >= _DATA_SPACE or kind not in (_STT_NOTYPE, _STT_FUNC)
                    or not secs[shndx][2] & _SHF_EXECINSTR):     # code only (no debug labels)
                continue
            end = strtab.find(b"\0", nm)
            name = strtab[nm:end if end >= 0 else None].decode("ascii", "replace")
            if name and not name.startswith(("$", ".L")):
                yield Symbol(value, name, sz)
