"""Disassembler checks: exhaustive cross-check against avr-objdump (skipped without it)."""

import re
import shutil
import struct
import subprocess

import pytest

from simavr_py.disasm import decode, disassemble, window

# Opcodes avr-objdump knows but no simavr core implements.
UNIMPLEMENTED = {"xch", "las", "lac", "lat", "des"}


def test_known_encodings():
    assert decode(0x0000, None, 0).text == "nop"
    assert decode(0xE00A, None, 0).text == "ldi r16, 0x0A"
    assert decode(0x940C, 0x1234, 0).text == "jmp 0x2468"
    assert decode(0x940C, 0x1234, 0).target == 0x2468
    assert decode(0x940C, None, 0).text == ".word 0x940c"       # cut off
    assert decode(0xCFFF, None, 0x10).text == "rjmp 0x0010"     # to itself
    assert decode(0xCFFE, None, 0, flash_size=0x100).target == 0xFE   # wraps below 0
    assert decode(0xF409, None, 0x20).text == "brne 0x0024"
    assert decode(0x9508, None, 0).text == "ret"
    assert decode(0x9A01, None, 0).text == "sbi 0x00, 1"


def test_disassemble_sizes():
    prog = struct.pack("<HHHH", 0x9100, 0x0800, 0x0000, 0x9508)     # lds r16,0x0800; nop; ret
    out = disassemble(prog, base=0x100)
    assert [(i.addr, i.size, i.text) for i in out] == [
        (0x100, 4, "lds r16, 0x0800"), (0x104, 2, "nop"), (0x106, 2, "ret")]


def test_window_syncs_on_pc():
    # ldi, then a 4-byte lds, then nops: windows starting mid-lds must resync.
    flash = struct.pack("<HHH", 0xE001, 0x9100, 0x0800) + b"\x00\x00" * 40

    def read(a, n):
        return flash[a:a + n]
    for pc in (0, 6, 8, 40):
        rows = window(read, pc, len(flash), before=3, after=3)
        assert any(i.addr == pc for i in rows)
    assert window(read, len(flash) + 8, len(flash)) == []


@pytest.mark.skipif(not shutil.which("avr-objdump"), reason="avr-objdump not installed")
def test_every_opcode_matches_objdump(tmp_path):
    data = b"".join(struct.pack("<HH", w, 0) for w in range(0x10000))   # opcode, then nop
    path = tmp_path / "all.bin"
    path.write_bytes(data)
    r = subprocess.run(["avr-objdump", "-D", "-b", "binary", "-m", "avr:6", str(path)],
                       capture_output=True, text=True, check=True)
    ref = {}
    for line in r.stdout.splitlines():
        m = re.match(r"\s*([0-9a-f]+):\t[0-9a-f ]+?\s*\t(.*)", line)
        if m:
            ref[int(m[1], 16)] = re.sub(r"\s+", " ", m[2].split(";")[0]).strip()
    mine = {i.addr: i for i in disassemble(data)}
    checked = 0
    for addr, want in ref.items():
        got = mine[addr]
        if want.split()[0] in UNIMPLEMENTED:
            assert got.text.startswith(".word"), (hex(addr), want, got.text)
            continue
        if got.target is not None:
            op, _, operand = want.partition(" ")
            if operand.startswith("."):
                want_t = (addr + 2 + int(operand[1:])) & 0xFFFFFF       # objdump: ".+N" is from addr + 2
                assert got.text == f"{op} 0x{want_t:04x}", (hex(addr), want, got.text)
            else:
                assert got.target == int(operand, 0), (hex(addr), want, got.text)
        else:
            assert got.text == want, (hex(addr), want, got.text)
        checked += 1
    assert checked > 60000
