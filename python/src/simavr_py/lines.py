"""DWARF line-number reader (Qt-free): flash address -> source file:line.

Reads `.debug_line` (DWARF 2-5, 32-bit DWARF, little-endian ELF32 - what
avr-gcc emits) from the linked ELF, where addresses are already relocated.
Like `symbols`, this is decoration: anything malformed or unsupported yields an
empty table rather than an error.
"""

from __future__ import annotations

import os
import struct
from bisect import bisect_right
from dataclasses import dataclass

_DW_LNCT_PATH, _DW_LNCT_DIRECTORY_INDEX = 1, 2
_FORM_STRING, _FORM_LINE_STRP, _FORM_STRP = 0x08, 0x1F, 0x0E
_FORM_UDATA, _FORM_DATA1, _FORM_DATA2 = 0x0F, 0x0B, 0x05
_FORM_DATA4, _FORM_DATA8, _FORM_DATA16, _FORM_BLOCK = 0x06, 0x07, 0x1E, 0x09


@dataclass(frozen=True)
class Line:
    file: str
    line: int


class LineTable:
    def __init__(self, rows=()):
        # rows: (addr, Line | None); None ends a sequence. Sorted by address,
        # a later row at the same address wins (end markers sort first).
        ordered = sorted(enumerate(rows), key=lambda t: (t[1][0], t[0]))
        self._addrs = [r[0] for _, r in ordered]
        self._lines = [r[1] for _, r in ordered]

    def __len__(self) -> int:
        return sum(1 for ln in self._lines if ln is not None)

    def find(self, addr: int) -> Line | None:
        i = bisect_right(self._addrs, addr) - 1
        return self._lines[i] if i >= 0 else None


def read_elf_lines(path: str) -> LineTable:
    try:
        with open(path, "rb") as f:
            return LineTable(_parse(f.read()))
    except (OSError, struct.error, ValueError, IndexError, KeyError):
        return LineTable()


def _sections(blob: bytes) -> dict[str, bytes]:
    if blob[:4] != b"\x7fELF" or blob[4] != 1 or blob[5] != 1:
        return {}
    shoff, = struct.unpack_from("<I", blob, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", blob, 0x2E)
    if not shoff or shentsize < 40:
        return {}
    secs = [struct.unpack_from("<IIIIIIIIII", blob, shoff + i * shentsize)
            for i in range(shnum)]
    names = blob[secs[shstrndx][4]:secs[shstrndx][4] + secs[shstrndx][5]]
    out = {}
    for nm, typ, flags, _a, off, size, *_ in secs:
        end = names.find(b"\0", nm)
        name = names[nm:end].decode("ascii", "replace")
        if typ != 8 and not flags & 0x800:      # skip NOBITS and SHF_COMPRESSED
            out[name] = blob[off:off + size]
    return out


class _Cur:
    def __init__(self, data: bytes, pos: int = 0):
        self.d, self.p = data, pos

    def u(self, n: int) -> int:
        v = int.from_bytes(self.d[self.p:self.p + n], "little")
        if self.p + n > len(self.d):
            raise ValueError("truncated")
        self.p += n
        return v

    def uleb(self) -> int:
        v = shift = 0
        while True:
            b = self.u(1)
            v |= (b & 0x7F) << shift
            shift += 7
            if not b & 0x80:
                return v

    def sleb(self) -> int:
        v = shift = 0
        while True:
            b = self.u(1)
            v |= (b & 0x7F) << shift
            shift += 7
            if not b & 0x80:
                return v - (1 << shift) if b & 0x40 else v

    def cstr(self) -> bytes:
        end = self.d.index(b"\0", self.p)
        s, self.p = self.d[self.p:end], end + 1
        return s


def _string_at(sec: bytes, off: int) -> str:
    return sec[off:sec.index(b"\0", off)].decode("utf-8", "replace")


def _entry_formats(c: _Cur):
    return [(c.uleb(), c.uleb()) for _ in range(c.u(1))]


def _read_entries(c: _Cur, fmts, secs):
    """DWARF 5 directory/file entry list -> [(path, dir_index)]."""
    out = []
    for _ in range(c.uleb()):
        path, di = "", 0
        for ct, form in fmts:
            if form == _FORM_STRING:
                val = c.cstr().decode("utf-8", "replace")
            elif form in (_FORM_LINE_STRP, _FORM_STRP):
                sec = secs.get(".debug_line_str" if form == _FORM_LINE_STRP else ".debug_str", b"")
                val = _string_at(sec, c.u(4))
            elif form == _FORM_UDATA:
                val = c.uleb()
            elif form in (_FORM_DATA1, _FORM_DATA2, _FORM_DATA4, _FORM_DATA8):
                val = c.u({_FORM_DATA1: 1, _FORM_DATA2: 2, _FORM_DATA4: 4, _FORM_DATA8: 8}[form])
            elif form == _FORM_DATA16:
                val = c.u(16)
            elif form == _FORM_BLOCK:
                val = c.u(c.uleb())
            else:
                raise ValueError(f"unsupported form {form:#x}")
            if ct == _DW_LNCT_PATH:
                path = val
            elif ct == _DW_LNCT_DIRECTORY_INDEX:
                di = val
        out.append((path, di))
    return out


def _parse(blob: bytes):
    secs = _sections(blob)
    data = secs.get(".debug_line", b"")
    pos = 0
    while pos + 4 <= len(data):
        length, = struct.unpack_from("<I", data, pos)
        if length >= 0xFFFFFFF0:
            return                              # 64-bit DWARF: unsupported
        unit_end = pos + 4 + length
        try:
            yield from _unit(data[:unit_end], pos + 4, secs)
        except (ValueError, IndexError, struct.error):
            pass                                # skip a bad unit, keep the rest
        pos = unit_end


def _unit(data: bytes, pos: int, secs):
    c = _Cur(data, pos)
    version = c.u(2)
    if not 2 <= version <= 5:
        return
    if version >= 5:
        c.u(2)                                  # address_size, seg_selector_size
    hdr_len = c.u(4)
    prog = c.p + hdr_len
    min_inst = c.u(1)
    if version >= 4:
        c.u(1)                                  # max ops per instruction (VLIW)
    c.u(1)                                      # default_is_stmt
    line_base = c.u(1)
    line_base -= 256 if line_base > 127 else 0
    line_range, opcode_base = c.u(1), c.u(1)
    oplens = [c.u(1) for _ in range(opcode_base - 1)]

    if version >= 5:
        dirs = [p for p, _ in _read_entries(c, _entry_formats(c), secs)]
        files = _read_entries(c, _entry_formats(c), secs)
    else:
        dirs = [""]
        while data[c.p]:
            dirs.append(c.cstr().decode("utf-8", "replace"))
        c.p += 1
        files = [("", 0)]                       # file numbers are 1-based
        while data[c.p]:
            name = c.cstr().decode("utf-8", "replace")
            files.append((name, c.uleb()))
            c.uleb(), c.uleb()                  # mtime, length
    names = {}

    def file_name(i: int) -> str:
        if i not in names:
            n, di = files[i] if 0 <= i < len(files) else ("?", 0)
            d = dirs[di] if 0 <= di < len(dirs) else ""
            names[i] = n if os.path.isabs(n) or not d else os.path.join(d, n)
        return names[i]

    c.p = prog
    # DWARF 5 file numbers are 0-based; earlier versions 1-based (index 0 unused above).
    reg = lambda: [0, 1, 1]                     # address, file, line
    addr, fil, line = reg()
    emit = lambda: (addr, Line(file_name(fil), line))
    while c.p < len(data):
        op = c.u(1)
        if op >= opcode_base:
            adj = op - opcode_base
            addr += (adj // line_range) * min_inst
            line += line_base + adj % line_range
            yield emit()
        elif op == 0:
            n = c.uleb()
            end = c.p + n
            sub = c.u(1)
            if sub == 1:                        # end_sequence
                yield addr, None
                addr, fil, line = reg()
            elif sub == 2:                      # set_address
                addr = c.u(n - 1)
            c.p = end
        elif op == 1:
            yield emit()
        elif op == 2:
            addr += c.uleb() * min_inst
        elif op == 3:
            line += c.sleb()
        elif op == 4:
            fil = c.uleb()
        elif op == 5 or op == 12:
            c.uleb()
        elif op == 8:
            addr += ((255 - opcode_base) // line_range) * min_inst
        elif op == 9:
            addr += c.u(2)
        else:                                   # 6,7,10,11 take no args; others skip by table
            for _ in range(oplens[op - 1] if op not in (6, 7, 10, 11) else 0):
                c.uleb()
