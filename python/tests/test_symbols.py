import shutil
import subprocess

import pytest

from simavr_py.symbols import Symbol, SymbolTable, read_elf_symbols


def test_find_and_describe():
    t = SymbolTable([Symbol(0, "__vectors", 0), Symbol(0x20, "foo", 8),
                     Symbol(0x30, "label", 0)])
    assert t.describe(0) == "__vectors"
    assert t.describe(0x1E) == "__vectors+0x1e"        # size-0 label runs on
    assert t.describe(0x24) == "foo+0x4"
    assert t.describe(0x28) == ""                      # past foo's extent
    assert t.describe(0x31) == "label+0x1"
    assert SymbolTable().describe(4) == ""


def test_prefers_sized_symbol_at_same_address():
    t = SymbolTable([Symbol(8, "__alias", 0), Symbol(8, "main", 12)])
    assert t.describe(8) == "main"


def test_unreadable_files_give_empty_table(tmp_path):
    bad = tmp_path / "x.elf"
    bad.write_bytes(b"\x7fELF\x01\x01\x01" + b"\0" * 20)
    assert len(read_elf_symbols(str(bad))) == 0
    assert len(read_elf_symbols(str(tmp_path / "missing.elf"))) == 0


def test_reads_avr_gcc_elf(tmp_path):
    if not shutil.which("avr-gcc"):
        pytest.skip("avr-gcc not installed")
    src, elf = tmp_path / "fw.c", tmp_path / "fw.elf"
    src.write_text("volatile int x; void foo(void){x++;}"
                   " int main(void){for(;;)foo();}")
    r = subprocess.run(["avr-gcc", "-mmcu=atmega4809", "-Os", "-o", elf, src],
                       capture_output=True)
    if r.returncode:
        pytest.skip("avr-gcc lacks atmega4809")
    t = read_elf_symbols(str(elf))
    main, foo = t.lookup("main"), t.lookup("foo")
    assert main and foo and foo.size > 0
    assert t.describe(0) == "__vectors"
    assert t.describe(foo.addr + 2) == "foo+0x2"
    assert not any(n.startswith(".L") for n in t._by_name)
