"""Interrupt vector names (Qt-free), read from the device's avr-libc header.

The names come from the `<NAME>_vect_num` defines in `io<device>.h`, looked up in
this checkout's `simavr/cores/avr/` first, then in an `include/avr` directory next
to an installed `avr-gcc` (and `$AVR_INCLUDE`). Vector names are decoration: a
device or header that can't be found yields an empty table, never an error.
"""

from __future__ import annotations

import os
import re
import shutil
from functools import lru_cache
from pathlib import Path

_DEFINE = re.compile(r"^\s*#\s*define\s+(\w+)_vect_num\s+(\d+)\b", re.M)
_PREFIXES = (("attiny", "iotn"), ("atmega", "iom"))


def header_name(mmcu: str) -> str | None:
    m = mmcu.lower()
    for prefix, stem in _PREFIXES:
        if m.startswith(prefix) and m[len(prefix):].isalnum():
            return f"{stem}{m[len(prefix):]}.h"
    return None


def _search_dirs():
    here = Path(__file__).resolve()
    for parent in here.parents:
        d = parent / "simavr" / "cores" / "avr"
        if d.is_dir():
            yield d
            break
    if os.environ.get("AVR_INCLUDE"):
        yield Path(os.environ["AVR_INCLUDE"])
    gcc = shutil.which("avr-gcc")
    if gcc:
        root = Path(gcc).resolve().parent.parent
        yield root / "avr" / "include" / "avr"
        yield root / "include" / "avr"


@lru_cache(maxsize=None)
def vector_names(mmcu: str) -> dict[int, str]:
    """{vector number: name} (vector 0 is RESET); empty when unknown."""
    name = header_name(mmcu or "")
    if name is None:
        return {}
    for d in _search_dirs():
        try:
            text = (d / name).read_text(errors="replace")
        except OSError:
            continue
        found = {int(n): v for v, n in _DEFINE.findall(text)}
        if found:
            found.setdefault(0, "RESET")
            return found
    return {}


def vector_label(names: dict[int, str], vector: int) -> str:
    return f"{vector} {names[vector]}" if vector in names else str(vector)
