"""Event System routing of a modern AVR (Qt-free), decoded from the device's avr-libc header.

`evsys(mmcu)` lists the channels (generator selection registers) and users (the peripherals
listening) with the data-space address of each register; `Evsys.decode_channel/decode_user`
turn a live register byte into the generator name and the channel a user is wired to.

Two register layouts exist, found by their register names:

* `CHANNELn` + `USER<name>` (megaAVR-0, tinyAVR 2-series): a user value of 0 is OFF and `n`
  selects CHANNEL(n-1) (megaAVR-0: DS40002173C 14.5.3). The 2-series follows its header's
  `EVSYS_USER_CHANNELn` enum; no 2-series data sheet was available to check it against.
* `ASYNCCHn`/`SYNCCHn` + `ASYNCUSERn`/`SYNCUSERn` (tinyAVR 0- and 1-series): a user value of
  0 is OFF, 1-2 are SYNCCH0-1 and 3-6 are ASYNCCH0-3 (DS40002205A 14.5.5/14.5.6).

Generator names are the header's `EVSYS_<register>_<NAME>_gc` enum entries (or the shared
`EVSYS_GENERATOR_` set of the megaAVR-0), user names come from the header's comments. A
header that can't be read gives no channels. Display only: values come from the registers.
"""

from __future__ import annotations

import re
from collections.abc import Callable
from dataclasses import dataclass
from functools import lru_cache

from .registers import registers
from .vectors import _search_dirs, header_name

_STRUCT = re.compile(r"typedef\s+struct\s+EVSYS_struct\s*\{(.*?)\}\s*EVSYS_t\s*;", re.S)
_MEMBER = re.compile(r"register8_t\s+(\w+)\s*;\s*/\*\s*(.*?)\s*\*/")
_ENUM = re.compile(r"EVSYS_(\w+?)_gc\s*=\s*\(\s*(0x[0-9A-Fa-f]+|\d+)\s*<<\s*0\s*\)")


@dataclass(frozen=True)
class Slot:
    name: str           # "CHANNEL0", "USERTCA0", "ASYNCUSER2"
    label: str          # short display name: "CHANNEL0", "TCA0", "CCL LUT0 Event 0"
    address: int


@dataclass(frozen=True)
class Evsys:
    layout: str                         # "channel" (mega-0 / tiny 2) | "async" (tiny 0/1)
    channels: tuple[Slot, ...]
    users: tuple[Slot, ...]
    generators: dict[str, dict[int, str]]       # channel register name -> {value: NAME}
    wired: tuple[str, ...]              # user value v (1-based) -> channel name; [0] unused

    def decode_channel(self, ch: Slot, value: int) -> str:
        if value == 0:
            return "OFF"
        return self.generators.get(ch.name, {}).get(value, f"0x{value:02X}")

    def decode_user(self, value: int) -> str:
        """Channel register name the user listens to, "OFF", or "?" for a value that selects none."""
        if value == 0:
            return "OFF"
        return self.wired[value - 1] if 0 < value <= len(self.wired) else "?"


@lru_cache(maxsize=None)
def evsys(mmcu: str) -> Evsys | None:
    """EVSYS layout of `mmcu`, or None if it has no readable EVSYS."""
    name = header_name(mmcu or "")
    text = ""
    for d in _search_dirs():
        try:
            text = (d / name).read_text(errors="replace") if name else ""
        except OSError:
            continue
        break
    m = _STRUCT.search(text)
    regs = {r.name.removeprefix("EVSYS."): r.address for r in registers(mmcu)
            if r.name.startswith("EVSYS.")}
    if not m or not regs:
        return None
    comments = dict(_MEMBER.findall(m[1]))
    enums: dict[str, dict[int, str]] = {}
    for full, val in _ENUM.findall(text):
        grp, _, opt = full.partition("_")
        enums.setdefault(grp, {})[int(val, 0)] = opt

    def slots(pred, label) -> tuple[Slot, ...]:
        return tuple(Slot(n, label(n), a) for n, a in sorted(regs.items(), key=lambda kv: kv[1])
                     if pred(n))

    if any(n.startswith("ASYNCCH") for n in regs):
        chan = lambda n: re.fullmatch(r"(?:A?SYNC)CH\d+", n) is not None
        user = lambda n: re.fullmatch(r"(?:A?SYNC)USER\d+", n) is not None
        # the header comment ends "... - TCB0"; the sync ones read "Synchronous User Ch 0 - TCA0"
        label = lambda n: comments.get(n, n).rsplit(" - ", 1)[-1]
        wired = ("SYNCCH0", "SYNCCH1", "ASYNCCH0", "ASYNCCH1", "ASYNCCH2", "ASYNCCH3")
        layout = "async"
    else:
        chan = lambda n: re.fullmatch(r"CHANNEL\d+", n) is not None
        user = lambda n: n.startswith("USER")
        label = lambda n: n.removeprefix("USER")
        wired = ()
        layout = "channel"
    channels = slots(chan, lambda n: n)
    users = slots(user, label)
    if layout == "channel":
        wired = tuple(c.name for c in channels)
    shared = enums.get("GENERATOR", {})
    gens = {c.name: enums.get(c.name, shared) for c in channels}
    return Evsys(layout, channels, users, gens, wired)


Read = Callable[[int], int]     # address -> byte


def rows(mmcu: str, read: Read) -> tuple[tuple[str, str, str, str], ...]:
    """(kind, name, value, meaning) for every channel then every user; empty if no EVSYS.
    A channel's meaning is its generator, a user's the channel it listens to (with that
    channel's generator)."""
    ev = evsys(mmcu)
    if ev is None:
        return ()
    by_name = {c.name: c for c in ev.channels}
    values = {c.name: read(c.address) for c in ev.channels}
    out = []
    for c in ev.channels:
        v = values[c.name]
        out.append(("Channel", c.label, f"0x{v:02X}", ev.decode_channel(c, v)))
    for u in ev.users:
        v = read(u.address)
        target = ev.decode_user(v)
        if target in by_name:
            target = f"{target}  ({ev.decode_channel(by_name[target], values[target])})"
        out.append(("User", u.label, f"0x{v:02X}", target))
    return tuple(out)
