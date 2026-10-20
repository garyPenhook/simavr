"""System-block register state of a modern AVR (Qt-free), decoded from the device's avr-libc header.

Same decoding as `analog` (register value plus every header-defined field, no table here), for the
blocks that are neither analog, serial, timer, event nor clock: reset and sleep control, brown-out
and watchdog, CPUINT, CRC scan, RTC/PIT, CCL, NVM controller and SYSCFG. `system(mmcu)` lists the
instances the device has; `decode(...)` turns live register bytes into rows. No derived values are
computed: a register view that never changes simulator state.
"""

from __future__ import annotations

import re
from functools import lru_cache

from .analog import Block, Read, _header, describe
from .registers import registers

KINDS = ("RSTCTRL", "SLPCTRL", "BOD", "WDT", "CPUINT", "CRCSCAN", "RTC", "CCL", "NVMCTRL", "SYSCFG")


@lru_cache(maxsize=None)
def system(mmcu: str) -> tuple[Block, ...]:
    """System block instances of `mmcu`, in `KINDS` order."""
    found: dict[str, dict[str, tuple[int, int]]] = {}
    for r in registers(mmcu):
        m = re.fullmatch(r"(\w+?)\.(\w+)", r.name)
        if m and m[1] in KINDS:
            found.setdefault(m[1], {})[m[2]] = (r.address, r.size)
    return tuple(Block(k, k, dict(sorted(found[k].items(), key=lambda kv: kv[1]))) for k in KINDS if k in found)


def decode(mmcu: str, block: Block, read: Read) -> tuple[tuple[str, str, str], ...]:
    """(register, "0x.." value, fields) per register of `block`. Empty if the header is unreadable."""
    h = _header(mmcu)
    if h is None:
        return ()
    out = []
    for reg, (addr, size) in block.regs.items():
        value = read(addr, size)
        out.append((reg, f"0x{value:0{2 * size}X}", describe(h.fields.get((block.kind, reg), ()), value)))
    return tuple(out)
