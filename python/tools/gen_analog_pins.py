"""Regenerate src/simavr_py/analog_pins.json: which pad each ADC / AC input index is on, per device.

Source: the `<signals>` of the ADC and AC instances in Microchip's ATDF device files (the
ATtiny_DFP and ATmega_DFP packs), which list only the pads the device really has.
    python tools/gen_analog_pins.py <atdf-dir> [<atdf-dir> ...]   (devices come from the cores)
"""

import json
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "python" / "src" / "simavr_py" / "analog_pins.json"


def devices():
    """Modern cores: those whose descriptor includes a modern template header."""
    for f in sorted((ROOT / "simavr" / "cores").glob("sim_*.c")):
        if any(t in f.read_text() for t in ("sim_tinyx1.h", "sim_tinyx2.h", "sim_megax08.h")):
            name = f.stem.removeprefix("sim_")
            yield ("attiny" if name.startswith("tiny") else "atmega") + name.removeprefix("tiny").removeprefix("mega")


def pins(atdf: Path) -> dict:
    out: dict = {}
    for inst in ET.parse(atdf).getroot().iter("instance"):
        n = inst.get("name", "")
        if n.rstrip("0123456789") not in ("ADC", "AC"):
            continue
        for s in inst.iter("signal"):
            out.setdefault(n, {}).setdefault(s.get("group"), {})[s.get("index")] = s.get("pad")
    return out


def main(dirs):
    files = {p.stem.lower(): p for d in dirs for p in Path(d).rglob("*.atdf")}
    result, missing = {}, []
    for dev in devices():
        if dev in files:
            result[dev] = pins(files[dev])
        else:
            missing.append(dev)
    if missing:
        print(f"no ATDF for (left out, no pin names shown): {' '.join(missing)}")
    OUT.write_text(json.dumps(result, indent=1, sort_keys=True) + "\n")
    print(f"{len(result)} devices -> {OUT}")


if __name__ == "__main__":
    main(sys.argv[1:] or sys.exit(__doc__))
