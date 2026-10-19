import re
import shutil
import subprocess

import pytest

from simavr_py.lines import Line, LineTable, read_elf_lines

SRC = "volatile int x;\nvoid foo(void){\n  x++;\n}\nint main(void){\n  for(;;)\n    foo();\n}\n"


def test_find_and_sequence_end():
    t = LineTable([(0x10, Line("a.c", 1)), (0x14, Line("a.c", 2)), (0x18, None),
                   (0x30, Line("b.c", 9)), (0x34, None)])
    assert t.find(0x0F) is None
    assert t.find(0x10).line == 1 and t.find(0x13).line == 1
    assert t.find(0x14).line == 2
    assert t.find(0x18) is None and t.find(0x2F) is None      # gap between sequences
    assert t.find(0x31).file == "b.c"
    assert t.find(0x34) is None
    assert len(t) == 3


def test_address_of_picks_lowest_address_then_next_code_line():
    t = LineTable([(0x20, Line("a.c", 5)), (0x10, Line("a.c", 5)), (0x14, Line("a.c", 7)),
                   (0x18, None), (0x30, Line("b.c", 6)), (0x34, None)])
    assert t.address_of("a.c", 5) == 0x10
    assert t.address_of("a.c", 6) == 0x14           # blank line -> next line with code
    assert t.address_of("a.c", 8) is None
    assert t.address_of("b.c", 1) == 0x30
    assert t.address_of("c.c", 1) is None
    assert t.lines_with_code("a.c") == {5, 7}


def test_unreadable_files_give_empty_table(tmp_path):
    bad = tmp_path / "x.elf"
    bad.write_bytes(b"\x7fELF\x01\x01\x01" + b"\0" * 20)
    assert len(read_elf_lines(str(bad))) == 0
    assert len(read_elf_lines(str(tmp_path / "missing.elf"))) == 0


@pytest.mark.parametrize("version", [2, 3, 4, 5])
def test_matches_objdump(tmp_path, version):
    if not shutil.which("avr-gcc") or not shutil.which("avr-objdump"):
        pytest.skip("avr toolchain not installed")
    src, elf = tmp_path / "fw.c", tmp_path / "fw.elf"
    src.write_text(SRC)
    r = subprocess.run(["avr-gcc", "-mmcu=atmega4809", "-Os", "-g", f"-gdwarf-{version}",
                        "-o", elf, src], capture_output=True)
    if r.returncode:
        pytest.skip("avr-gcc lacks atmega4809")
    t = read_elf_lines(str(elf))
    assert len(t) > 0
    # Every address objdump lists must resolve to the same file:line here.
    out = subprocess.run(["avr-objdump", "--dwarf=decodedline", str(elf)],
                         capture_output=True, text=True).stdout
    last = {}                       # several rows may share an address: the last wins
    for m in re.finditer(r"^(\S+)\s+(\d+)\s+0x([0-9a-f]+)", out, re.M):
        last[int(m.group(3), 16)] = (m.group(1), int(m.group(2)))
    checked = 0
    for addr, (name, line) in last.items():
        got = t.find(addr)
        assert got is not None and got.file.endswith(name) and got.line == line, (name, line, addr, got)
        checked += 1
    assert checked > 5
    assert {t.find(a).line for a in range(0, 0x400) if t.find(a) and t.find(a).file.endswith("fw.c")} >= {3, 7}
