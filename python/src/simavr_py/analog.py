"""ADC / VREF / AC / DAC register state of a modern AVR (Qt-free), decoded from the device's avr-libc header.

`analog(mmcu)` lists the analog peripheral instances (`ADC0`, `VREF`, `AC1`, `DAC0`, ...) with the
data-space address of each register; `decode(mmcu, block, read)` turns the live register bytes
into one row per register: its value and every field the header defines for it. The fields of a
register are the `_gm`/`_bm` masks that follow its `/* ADC.CTRLA  bit masks ... */` banner in the
header, and a field's option name is the header's `<TYPE>_<FIELD>_<OPTION>_gc` enum entry, so each
family's own layout (the tinyAVR 2-series ADC with its PGA and MUXNEG, the tinyAVR 1-series VREF
with four registers) is followed without a table here. A multi-bit field with no enum shows its
number, a single bit shows its name when set, and registers without bit definitions (RES, SAMPLE,
WINLT, DATA ...) show only their value.

Beyond the raw fields each block gets derived rows (data-sheet arithmetic, not simulation):

* VREF: a reference option shows its nominal voltage. The option name carries it (`2V5`, `1V024`),
  except the megaAVR-0 / tinyAVR 0/1 `4V34`, which the data sheets give as 4.3 V (4.34 V there
  is only a BOD threshold).
* ADC: the selected reference and the last result as volts. megaAVR-0 / tinyAVR 0/1:
  RES = 1023 * VIN / VREF (DS40002173C 29.3.2.5), accumulated over SAMPNUM samples, 8-bit mode
  keeps the top eight of the ten bits. tinyAVR 2-series: the SAMPLE register by the output
  formats of DS40002345B 30.3.3.3 (12/8-bit, single-ended/differential, PGA gain, LEFTADJ).
* ADC temperature sensor (tinyAVR 0/1, MUXPOS = TEMPSENSE): kelvin and degrees C from RES and the
  SIGROW.TEMPSENSE0 (gain, unsigned) / TEMPSENSE1 (offset, signed) calibration bytes, by the code in
  DS40002205A 30.3.2.6: K = ((RES - TEMPSENSE1) * TEMPSENSE0 + 0x80) >> 8. The data sheet needs the
  1.1 V reference; with another one the row says so instead of a temperature.
* DAC: the output as DATA * VREF / 256 for the reference VREF.DACnREFSEL selects. DS40002205A 31.3.1
  says only that the range is GND to the selected reference with 8-bit data (31.5.2, Table 36-31) and
  never prints the divisor, so the row is labelled nominal (the AC's divider, 28.5.3, does print /256).
* AC: the DACREF divider output, DACREF * VREF / 256 (DS40002173C 29.5.4).
* ADC and AC mux options name the pad (`MUXPOS=AIN3 (PD3)`) from `analog_pins.json`, generated
  from Microchip's ATDF device files by `tools/gen_analog_pins.py`.

VDD (supply) isn't known to the simulator's registers, so the caller passes it (`vdd_mv`).
An external VREFA has no known voltage, so no result is computed for it. Everything is a
register view: it never changes simulator state.
"""

from __future__ import annotations

import json
import re
from collections.abc import Callable
from dataclasses import dataclass
from functools import lru_cache
from pathlib import Path

from .classic import classic, fields as classic_fields, group_of
from .registers import registers
from .vectors import _search_dirs, header_name

KINDS = ("ADC", "VREF", "AC", "DAC")

_BANNER = re.compile(r"/\*\s*(\w+)\.(\w+)\s+bit masks and bit positions\s*\*/")
_MASK = re.compile(r"^\s*#\s*define\s+(\w+?)_(gm|bm)\s+(0x[0-9A-Fa-f]+|\d+|\(1<<(\d+)\))", re.M)
_ENUM = re.compile(r"^\s*(\w+?)_gc\s*=\s*\(\s*(0x[0-9A-Fa-f]+|\d+)\s*<<\s*(\d+)\s*\)", re.M)
_SHARED = re.compile(r"/\*\s*(\w+)\s+is already defined\.\s*\*/")     # INTFLAGS reusing INTCTRL's bits
_BIT_N = re.compile(r"_\d+$")           # FIELD_0_bm: one bit of a group, covered by FIELD_gm


@dataclass(frozen=True)
class Field:
    name: str                       # "MUXPOS"
    mask: int
    options: dict[int, str]         # field value -> option name (empty: number or flag)


@dataclass(frozen=True)
class Block:
    name: str                       # "ADC0", "VREF"
    kind: str                       # "ADC" | "VREF" | "AC" | "DAC"
    regs: dict[str, tuple[int, int]]    # "CTRLA" -> (address, size), in address order


@dataclass(frozen=True)
class _Header:
    fields: dict[tuple[str, str], tuple[Field, ...]]    # (TYPE, REG) -> fields, low bit first


def _parse(text: str) -> _Header:
    banners = [(m.start(), m[1], m[2]) for m in _BANNER.finditer(text)]
    masks: dict[tuple[str, str], dict[str, int]] = {}
    for m in _MASK.finditer(text):
        here = [(t, r) for pos, t, r in banners if pos < m.start()]
        if not here or _BIT_N.search(m[1]) or not m[1].startswith(here[-1][0] + "_"):
            continue
        value = 1 << int(m[4]) if m[4] is not None else int(m[3], 0)
        masks.setdefault(here[-1], {}).setdefault(m[1].removeprefix(here[-1][0] + "_"), value)
    for m in _SHARED.finditer(text):
        here = [(t, r) for pos, t, r in banners if pos < m.start()]
        if not here:
            continue
        typ, name = here[-1][0], m[1].removeprefix(here[-1][0] + "_")
        owner = next((f[name] for (t, _), f in masks.items() if t == typ and name in f), 0)
        if owner:
            masks.setdefault(here[-1], {}).setdefault(name, owner)
    enums = [(m[1], int(m[2], 0) << int(m[3])) for m in _ENUM.finditer(text)]
    out: dict[tuple[str, str], tuple[Field, ...]] = {}
    for (typ, reg), fields in masks.items():
        built = []
        for name, mask in fields.items():
            shift = (mask & -mask).bit_length() - 1
            opts = {}
            for full, value in enums:
                if not full.startswith(f"{typ}_{name}_") or value & ~mask:
                    continue
                # a longer field name that also matches owns the option (MUXPOS vs MUXPOS_X)
                if any(full.startswith(f"{typ}_{o}_") and len(o) > len(name) for o in fields):
                    continue
                opts[value >> shift] = full[len(typ) + len(name) + 2:]
            built.append(Field(name, mask, opts))
        out[typ, reg] = tuple(sorted(built, key=lambda f: f.mask & -f.mask))
    return _Header(out)


def _classic_header(mmcu: str) -> _Header | None:
    """Fields of a classic AVR's registers, keyed (dock group, register) like the modern ones."""
    h = classic(mmcu)
    if h is None:
        return None
    return _Header({(group_of(n).upper(), n): tuple(Field(f, m, {}) for f, m in classic_fields(h, n))
                    for n, _, _ in h.regs})


def classic_blocks(mmcu: str, group: str, name: str) -> tuple[Block, ...]:
    """One block holding every register of a classic AVR that belongs to dock `group`."""
    h = classic(mmcu)
    regs = {n: (a, s) for n, a, s in h.regs if group_of(n) == group} if h else {}
    return (Block(name, group.upper(), regs),) if regs else ()


@lru_cache(maxsize=None)
def _header(mmcu: str) -> _Header | None:
    if classic(mmcu):
        return _classic_header(mmcu)
    name = header_name(mmcu or "")
    for d in _search_dirs():
        try:
            return _parse((d / name).read_text(errors="replace")) if name else None
        except OSError:
            continue
    return None


@lru_cache(maxsize=None)
def analog(mmcu: str) -> tuple[Block, ...]:
    """Analog peripheral instances of `mmcu`: ADC, VREF, AC, DAC, each by instance number.
    A classic AVR has one block with its ADC and analog comparator registers."""
    if classic(mmcu):
        return classic_blocks(mmcu, "analog", "ADC / AC")
    found: dict[str, dict[str, tuple[int, int]]] = {}
    for r in registers(mmcu):
        m = re.fullmatch(r"((?:ADC|VREF|AC|DAC)\d*)\.(\w+)", r.name)
        if m:
            found.setdefault(m[1], {})[m[2]] = (r.address, r.size)
    return tuple(Block(n, n.rstrip("0123456789"), dict(sorted(found[n].items(), key=lambda kv: kv[1])))
                 for n in sorted(found, key=lambda n: (KINDS.index(n.rstrip("0123456789")), len(n), n)))


Read = Callable[[int, int], int]    # (address, size) -> value


@lru_cache(maxsize=None)
def _pads(mmcu: str) -> dict:
    """{"ADC0": {"AIN": {"3": "PD3"}}, "AC0": {"P": {...}, "N": {...}}} from analog_pins.json."""
    try:
        return json.loads((Path(__file__).with_name("analog_pins.json")).read_text()).get(mmcu.lower(), {})
    except (OSError, ValueError):
        return {}


def pad(mmcu: str, block: Block, field: str, option: str) -> str:
    """Pad a mux option selects: "PD3", "-" if the device has no such pad, "" if not a pin option."""
    pads = _pads(mmcu).get(block.name)
    if not pads:
        return ""
    if block.kind == "ADC":
        m, group = re.fullmatch(r"AIN(\d+)", option), "AIN"
    elif block.kind == "AC" and field in ("MUXPOS", "MUXNEG"):
        group = "P" if field == "MUXPOS" else "N"
        m = re.fullmatch(r"(?:PIN|AINP|AINN)(\d+)", option)
    else:
        return ""
    return pads.get(group, {}).get(m[1], "-") if m else ""


def option_volts(option: str) -> float | None:
    """Nominal volts of a reference option name: `2V5`, `1V024`, `1024MV`; None for VDD/external."""
    if option == "4V34":                        # DS40002173C/205A/204A: 4.3 V (4.34 V is a BOD level)
        return 4.3
    if m := re.fullmatch(r"(\d)V(\d+)", option):
        return float(f"{m[1]}.{m[2]}")
    if m := re.fullmatch(r"(\d+)MV", option):
        return int(m[1]) / 1000
    return None


def describe(fields: tuple[Field, ...], value: int, note: Callable[[str, str], str] = lambda f, o: "") -> str:
    """Field list of one register value: `NAME=option`, `NAME=n`, or the name of a set bit.
    `note(field, option)` may add a suffix to an option."""
    parts = []
    for f in fields:
        v = (value & f.mask) >> ((f.mask & -f.mask).bit_length() - 1)
        if f.options:
            opt = f.options.get(v)
            parts.append(f"{f.name}={opt}" + (f" ({n})" if opt and (n := note(f.name, opt)) else "")
                         if opt else f"{f.name}={v}")
        elif f.mask & (f.mask - 1):
            parts.append(f"{f.name}={v}")
        elif v:
            parts.append(f.name)
    return "  ".join(parts) or "-"


class _View:
    """Field-level access to one device's analog registers through a `read`."""

    def __init__(self, mmcu: str, read: Read, vdd_mv: int):
        self.mmcu, self.read, self.vdd = mmcu, read, vdd_mv
        self.blocks = {b.name: b for b in analog(mmcu)}
        self.h = _header(mmcu)

    def raw(self, block: str, reg: str) -> int | None:
        b = self.blocks.get(block)
        return self.read(*b.regs[reg]) if b and reg in b.regs else None

    def opt(self, block: str, reg: str, name: str) -> tuple[int, str] | None:
        """(field value, option name or str(value)) of `name` in block.reg."""
        b, value = self.blocks.get(block), self.raw(block, reg)
        if b is None or value is None:
            return None
        for f in self.h.fields.get((b.kind, reg), ()):
            if f.name == name:
                v = (value & f.mask) >> ((f.mask & -f.mask).bit_length() - 1)
                return v, f.options.get(v, str(v))
        return None

    def vref_option(self, field: str) -> str | None:
        """Option selected by the VREF field called `field` (e.g. "ADC1REFSEL"), any VREF register."""
        for reg in self.blocks.get("VREF", Block("", "", {})).regs:
            if r := self.opt("VREF", reg, field):
                return r[1]
        return None

    def volts(self, option: str | None) -> float | None:
        """Volts of a reference option; VDD options use the supplied supply."""
        if option is None:
            return None
        if option.startswith(("VDD", "AVDD")):
            return self.vdd / 1000
        return option_volts(option)


@lru_cache(maxsize=None)
def _sigrow(mmcu: str) -> tuple[int, int] | None:
    """Data-space addresses of SIGROW.TEMPSENSE0 / TEMPSENSE1, None if the device lacks them."""
    a = {r.name: r.address for r in registers(mmcu)}
    return (a["SIGROW.TEMPSENSE0"], a["SIGROW.TEMPSENSE1"]) if "SIGROW.TEMPSENSE0" in a and "SIGROW.TEMPSENSE1" in a else None


def _temp_row(v: _View, b: Block, ref: str, n_samples: int, res: int) -> tuple[str, str] | None:
    """Temperature row for an ADC reading the on-chip sensor (tinyAVR 0/1), else None."""
    mux, cal = v.opt(b.name, "MUXPOS", "MUXPOS"), _sigrow(v.mmcu)
    if mux is None or mux[1] != "TEMPSENSE" or cal is None:
        return None
    if ref != "1V1":
        return ("Temperature", f"needs the 1.1 V reference (selected: {ref})")
    if n_samples != 1:
        return ("Temperature", "single-sample conversions only (SAMPNUM = ACC1)")
    gain, off = v.read(cal[0], 1), v.read(cal[1], 1)
    off -= 256 if off & 0x80 else 0                                 # TEMPSENSE1 is signed
    kelvin = (((res - off) * gain + 0x80) >> 8) if res >= off else 0   # user-code steps, DS40002205A p.434
    return ("Temperature", f"RES = {res}, gain {gain}, offset {off}  ->  {kelvin} K = {kelvin - 273.15:.1f} C")


def _dac_rows(v: _View, b: Block) -> list[tuple[str, str]]:
    if "DATA" not in b.regs:
        return []
    ref = v.vref_option(f"{b.name}REFSEL")
    volts = v.volts(ref)
    if volts is None:
        return []
    code, on = v.raw(b.name, "DATA"), (v.raw(b.name, "CTRLA") or 0) & 1
    text = f"{code} * {ref} ({volts:g} V) / 256 = {code * volts / 256:.4g} V  (nominal"
    return [("Output", text + ("" if on else ", DAC disabled") + ")")]


def _adc_rows(v: _View, b: Block) -> list[tuple[str, str]]:
    n = b.name.removeprefix("ADC")
    sel = v.opt(b.name, "CTRLC", "REFSEL")
    if sel is None:
        return []
    ref = sel[1]
    if ref == "INTREF":                         # internal: VREF.ADCnREFSEL picks the level
        ref = v.vref_option(f"ADC{n}REFSEL") or "INTREF"
    volts = v.volts(ref)
    if volts is None:
        text = f"{ref}  (external, voltage unknown)"
    else:
        text = f"{ref}  = {volts:g} V" + ("  (VDD as set)" if ref.startswith("VDD") else "")
    rows = [("Reference", text)]
    if volts is None:
        return rows
    if "SAMPLE" in b.regs:                      # tinyAVR 2-series
        cmd, pga = v.raw(b.name, "COMMAND"), v.raw(b.name, "PGACTRL") or 0
        mode, diff = v.opt(b.name, "COMMAND", "MODE"), bool((cmd or 0) & 0x80)
        if mode is None or v.raw(b.name, "SAMPLE") is None:
            return rows
        raw = v.raw(b.name, "SAMPLE")
        eight = mode[0] == 0
        gain, via = 1, v.opt(b.name, "MUXPOS", "VIA")
        if (pga & 1) and via and via[1] == "PGA":
            g = v.opt(b.name, "PGACTRL", "GAIN")
            gain = 2 ** g[0] if g and g[0] <= 4 else 1
        if not eight and (v.raw(b.name, "CTRLF") or 0) & 0x10:      # LEFTADJ: SAMPLE = code << 4
            raw >>= 4
        bits = 8 if eight else 12
        code = raw & ((1 << bits) - 1)
        if diff and code >> (bits - 1):
            code -= 1 << bits
        full = (1 << (bits - 1)) if diff else (1 << bits)
        what = "(VINP-VINN)" if diff else "VINP"
        rows.append(("Result", f"SAMPLE = {code} of {full}  ->  {what} = {code * volts / full / gain:.4g} V"
                               + (f"  (PGA {gain}x)" if gain > 1 else "")
                               + ("" if eight or mode[0] == 1 else "  (last sample of an accumulation)")))
    else:                                       # megaAVR-0 / tinyAVR 0/1
        res = v.raw(b.name, "RES")
        ten = (v.opt(b.name, "CTRLA", "RESSEL") or (0, "10BIT"))[1] == "10BIT"
        acc = v.opt(b.name, "CTRLB", "SAMPNUM")
        n_samples = int(acc[1][3:]) if acc and acc[1].startswith("ACC") and acc[1][3:].isdigit() else 1
        avg = res / n_samples
        vin = avg * (1 if ten else 4) * volts / 1023
        rows.append(("Result", f"RES = {res}" + (f" over {n_samples} samples (avg {avg:g})" if n_samples > 1 else "")
                               + f"  ->  VIN ~ {vin:.4g} V"
                               + ("" if ten else "  (8-bit: top 8 of 10 bits, +-4 LSB)")))
        if ten and (t := _temp_row(v, b, ref, n_samples, res)):
            rows.append(t)
    return rows


def _ac_rows(v: _View, b: Block) -> list[tuple[str, str]]:
    if "DACREF" not in b.regs:
        return []
    n = b.name.removeprefix("AC")
    ref = v.vref_option(f"AC{n}REFSEL")
    volts = v.volts(ref)
    if volts is None:
        return []
    code = v.raw(b.name, "DACREF")
    return [("DACREF voltage", f"{code} * {ref} ({volts:g} V) / 256 = {code * volts / 256:.4g} V")]


def decode(mmcu: str, block: Block, read: Read, vdd_mv: int = 3300) -> tuple[tuple[str, str, str], ...]:
    """(register, "0x.." value, fields) per register of `block`, then derived rows (value "").
    Empty if the header is unreadable. `vdd_mv` is the supply assumed for VDD references."""
    h = _header(mmcu)
    if h is None:
        return ()
    note = lambda f, o: ("no pad on this device" if (p := pad(mmcu, block, f, o)) == "-" else p if p else
                         f"{vv:g} V" if block.kind == "VREF" and (vv := option_volts(o)) is not None else "")
    out = []
    for reg, (addr, size) in block.regs.items():
        value = read(addr, size)
        out.append((reg, f"0x{value:0{2 * size}X}", describe(h.fields.get((block.kind, reg), ()), value, note)))
    v = _View(mmcu, read, vdd_mv)
    derived = _adc_rows(v, block) if block.kind == "ADC" else _ac_rows(v, block) if block.kind == "AC" else \
        _dac_rows(v, block) if block.kind == "DAC" else []
    return tuple(out) + tuple((label, "", text) for label, text in derived)
