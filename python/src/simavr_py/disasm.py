"""AVR disassembler (Qt-free): the AVRe+/AVRxt instruction set as avr-objdump prints it.

Branch, jump and call operands are shown as absolute byte addresses (objdump uses
`.+N`); `Insn.target` carries the address for the GUI. Opcodes that no core
implements (XCH/LAS/LAC/LAT, DES) come out as `.word 0xNNNN`, as does a 32-bit
opcode whose second word is cut off by the end of the data.
"""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass


@dataclass(frozen=True)
class Insn:
    addr: int               # flash byte address
    size: int               # 2 or 4 bytes
    text: str
    target: int | None = None       # absolute byte address for branches/jumps/calls


_SREG_SET = ("sec", "sez", "sen", "sev", "ses", "seh", "set", "sei")
_SREG_CLR = ("clc", "clz", "cln", "clv", "cls", "clh", "clt", "cli")
_BRBS = ("brcs", "breq", "brmi", "brvs", "brlt", "brhs", "brts", "brie")
_BRBC = ("brcc", "brne", "brpl", "brvc", "brge", "brhc", "brtc", "brid")
_NOARG = {0x0000: "nop", 0x9508: "ret", 0x9518: "reti", 0x9588: "sleep",
          0x9598: "break", 0x95A8: "wdr", 0x95C8: "lpm", 0x95D8: "elpm",
          0x95E8: "spm", 0x95F8: "spm Z+", 0x9409: "ijmp", 0x9419: "eijmp",
          0x9509: "icall", 0x9519: "eicall"}
_LD_MODE = {0x1: "Z+", 0x2: "-Z", 0x9: "Y+", 0xA: "-Y", 0xC: "X", 0xD: "X+", 0xE: "-X"}


def _signed(v: int, bits: int) -> int:
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def decode(word: int, next_word: int | None, addr: int,
           flash_size: int = 0) -> Insn:
    """Decode the instruction at byte `addr`. `next_word` is the following flash
    word (None at the end of the data). A nonzero `flash_size` (bytes) wraps
    relative targets the way the core does."""
    def rel(k: int, bits: int) -> int:
        t = addr + 2 + 2 * _signed(k, bits)
        return t % flash_size if flash_size else t & 0xFFFFFF

    def bad() -> Insn:
        return Insn(addr, 2, f".word 0x{word:04x}")

    d5 = (word >> 4) & 0x1F
    r5 = ((word >> 5) & 0x10) | (word & 0xF)
    d4 = 16 + ((word >> 4) & 0xF)
    k8 = ((word >> 4) & 0xF0) | (word & 0xF)
    op = word >> 12

    if word in _NOARG:
        return Insn(addr, 2, _NOARG[word])
    if op == 0:
        sub = (word >> 8) & 0xF
        if sub == 1:
            return Insn(addr, 2, f"movw r{((word >> 4) & 0xF) * 2}, r{(word & 0xF) * 2}")
        if sub == 2:
            return Insn(addr, 2, f"muls r{d4}, r{16 + (word & 0xF)}")
        if sub == 3:
            name = ("mulsu", "fmul", "fmuls", "fmulsu")[((word >> 6) & 2) | ((word >> 3) & 1)]
            return Insn(addr, 2, f"{name} r{16 + ((word >> 4) & 7)}, r{16 + (word & 7)}")
        name = {1: "cpc", 2: "sbc", 3: "add"}.get(sub >> 2)
        if name is None:
            return bad()
        return Insn(addr, 2, f"{name} r{d5}, r{r5}")
    if op == 1:
        name = ("cpse", "cp", "sub", "adc")[(word >> 10) & 3]
        return Insn(addr, 2, f"{name} r{d5}, r{r5}")
    if op == 2:
        name = ("and", "eor", "or", "mov")[(word >> 10) & 3]
        return Insn(addr, 2, f"{name} r{d5}, r{r5}")
    if 3 <= op <= 7:
        name = {3: "cpi", 4: "sbci", 5: "subi", 6: "ori", 7: "andi"}[op]
        return Insn(addr, 2, f"{name} r{d4}, 0x{k8:02X}")
    if op in (8, 10):                       # LDD/STD with Y or Z displacement
        q = ((word >> 8) & 0x20) | ((word >> 7) & 0x18) | (word & 7)
        ptr = "Y" if word & 8 else "Z"
        store = word & 0x200
        if q == 0:
            return Insn(addr, 2, f"st {ptr}, r{d5}" if store else f"ld r{d5}, {ptr}")
        return Insn(addr, 2, f"std {ptr}+{q}, r{d5}" if store else f"ldd r{d5}, {ptr}+{q}")
    if op == 9:
        return _decode_9(word, next_word, addr, d5, rel)
    if op == 0xB:
        a = ((word >> 5) & 0x30) | (word & 0xF)
        if word & 0x800:
            return Insn(addr, 2, f"out 0x{a:02x}, r{d5}")
        return Insn(addr, 2, f"in r{d5}, 0x{a:02x}")
    if op in (0xC, 0xD):
        t = rel(word & 0xFFF, 12)
        return Insn(addr, 2, f"{'rjmp' if op == 0xC else 'rcall'} 0x{t:04x}", t)
    if op == 0xE:
        return Insn(addr, 2, f"ldi r{d4}, 0x{k8:02X}")
    # op == 0xF
    if not word & 0x800:
        t = rel((word >> 3) & 0x7F, 7)
        names = _BRBC if word & 0x400 else _BRBS
        return Insn(addr, 2, f"{names[word & 7]} 0x{t:04x}", t)
    if word & 8:
        return bad()
    name = ("bld", "bst", "sbrc", "sbrs")[(word >> 9) & 3]
    return Insn(addr, 2, f"{name} r{d5}, {word & 7}")


def _decode_9(word, next_word, addr, d5, rel) -> Insn:
    def bad() -> Insn:
        return Insn(addr, 2, f".word 0x{word:04x}")

    low = word & 0xF
    sub = (word >> 8) & 0xF
    if sub in (0, 1, 2, 3):                 # 1001 00xd dddd xxxx: loads and stores
        store = sub >= 2
        if low == 0:
            if next_word is None:
                return bad()
            if store:
                return Insn(addr, 4, f"sts 0x{next_word:04x}, r{d5}")
            return Insn(addr, 4, f"lds r{d5}, 0x{next_word:04x}")
        if low == 0xF:
            return Insn(addr, 2, f"push r{d5}" if store else f"pop r{d5}")
        if not store and low in (4, 5, 6, 7):
            name = "lpm" if low < 6 else "elpm"
            return Insn(addr, 2, f"{name} r{d5}, Z{'+' if low & 1 else ''}")
        mode = _LD_MODE.get(low)
        if mode is None:
            return bad()
        return Insn(addr, 2, f"st {mode}, r{d5}" if store else f"ld r{d5}, {mode}")
    if sub in (4, 5):                       # 1001 010x: single-register ops, flags, jumps
        if (word & 0xFF0F) == 0x9408:
            s = (word >> 4) & 7
            return Insn(addr, 2, (_SREG_CLR if word & 0x80 else _SREG_SET)[s])
        if low in (0xC, 0xD, 0xE, 0xF):     # JMP/CALL k22: bit 0 of the opcode is k's top bit
            if next_word is None:
                return bad()
            k = (((word >> 3) & 0x3E) | (word & 1)) << 16 | next_word
            return Insn(addr, 4, f"{'call' if word & 2 else 'jmp'} 0x{k * 2:04x}", k * 2)
        if sub == 4 and low == 0xB:
            return bad()                    # DES
        name = {0: "com", 1: "neg", 2: "swap", 3: "inc", 5: "asr", 6: "lsr",
                7: "ror", 0xA: "dec"}.get(low)
        if name:
            return Insn(addr, 2, f"{name} r{d5}")
        return bad()
    if sub in (6, 7):                       # ADIW / SBIW
        k = ((word >> 2) & 0x30) | (word & 0xF)
        return Insn(addr, 2, f"{'sbiw' if sub == 7 else 'adiw'} r{24 + 2 * ((word >> 4) & 3)}, 0x{k:02x}")
    if sub in (8, 9, 0xA, 0xB):             # CBI / SBIC / SBI / SBIS
        name = ("cbi", "sbic", "sbi", "sbis")[sub - 8]
        return Insn(addr, 2, f"{name} 0x{(word >> 3) & 0x1F:02x}, {word & 7}")
    # sub 0xC-0xF: MUL
    r5 = ((word >> 5) & 0x10) | low
    return Insn(addr, 2, f"mul r{d5}, r{r5}")


def disassemble(data: bytes, base: int = 0, flash_size: int = 0) -> list[Insn]:
    """Linearly decode `data` (little-endian words) located at byte address `base`."""
    out: list[Insn] = []
    n = len(data) & ~1
    i = 0
    while i < n:
        word = data[i] | data[i + 1] << 8
        nxt = data[i + 2] | data[i + 3] << 8 if i + 4 <= n else None
        insn = decode(word, nxt, base + i, flash_size)
        out.append(insn)
        i += insn.size
    return out


def window(read: Callable[[int, int], bytes], pc: int, flash_size: int,
           before: int = 8, after: int = 16) -> list[Insn]:
    """Instructions around `pc`, with `pc`'s instruction guaranteed present.

    Variable-length code can't be decoded backwards, so decode forward from a
    guess a little before `pc` and shift the guess one word at a time until the
    stream lands exactly on `pc` (the rows before `pc` are then best-effort;
    those from `pc` on are exact). `read(address, length)` returns flash bytes."""
    pc &= ~1
    for back in (before * 4, *range(before * 4 - 2, -1, -2)):
        start = max(0, pc - back) & ~1
        stop = min(flash_size, pc + (after + 2) * 4)
        if stop <= pc:
            return []
        insns = disassemble(read(start, stop - start), start, flash_size)
        for idx, ins in enumerate(insns):
            if ins.addr == pc:
                return insns[max(0, idx - before):idx + after + 1]
            if ins.addr > pc:
                break
    return []
