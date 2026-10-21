"""ADC/VREF/AC/DAC decoding for every modern layout (no Qt, no firmware), then the Analog dock
against real firmware."""

import os
import subprocess
import sys

import pytest

from simavr_py.analog import analog, decode
from simavr_py.registers import registers

CORES = ["atmega4809", "attiny3217", "attiny3224", "attiny1614", "attiny412"]


def rows(mmcu, mem=None, vdd_mv=3300):
    """{"ADC0.CTRLC": (value, fields)} with unlisted registers reading 0; `mem` is by name."""
    regs = {r.name: r.address for r in registers(mmcu)}
    by_addr = {regs[n]: v for n, v in (mem or {}).items()}
    out = {}
    for b in analog(mmcu):
        for reg, value, fields in decode(mmcu, b, lambda a, s: by_addr.get(a, 0), vdd_mv):
            out[f"{b.name}.{reg}"] = (value, fields)
    return out


def test_unknown_device_has_none():
    assert analog("atmega328") == () and analog("") == ()


def test_instances():
    names = lambda m: [b.name for b in analog(m)]
    assert names("atmega4809") == ["ADC0", "VREF", "AC0"]
    assert names("attiny3217") == ["ADC0", "ADC1", "VREF", "AC0", "AC1", "AC2", "DAC0", "DAC1", "DAC2"]
    assert names("attiny412") == ["ADC0", "VREF", "AC0", "DAC0"]


@pytest.mark.parametrize("mmcu", CORES)
def test_reset_state_reads_clean(mmcu):
    r = rows(mmcu)
    assert r and all(f in ("-",) or "=" in f or v == "" for v, f in r.values()), r
    assert r["ADC0.CTRLA"][1] in ("-", "RESSEL=10BIT"), r["ADC0.CTRLA"]


def test_mega_fields():
    r = rows("atmega4809", {"ADC0.CTRLA": 0x03, "ADC0.CTRLC": 0x15, "ADC0.MUXPOS": 0x1E, "ADC0.INTFLAGS": 0x01,
                            "AC0.CTRLA": 0x41, "AC0.MUXCTRLA": 0x0B, "AC0.STATUS": 0x11})
    assert r["ADC0.CTRLA"] == ("0x03", "ENABLE  FREERUN  RESSEL=10BIT")
    assert r["ADC0.CTRLC"][1] == "PRESC=DIV64  REFSEL=VDDREF"
    assert r["ADC0.MUXPOS"][1] == "MUXPOS=TEMPSENSE"
    assert r["ADC0.INTFLAGS"][1] == "RESRDY"
    assert r["AC0.CTRLA"][1] == "ENABLE  HYSMODE=OFF  LPMODE=DIS  INTMODE=BOTHEDGE  OUTEN"
    assert r["AC0.MUXCTRLA"][1] == "MUXNEG=DACREF  MUXPOS=PIN1 (PD4)"
    assert r["AC0.STATUS"][1] == "CMP  STATE"


def test_sixteen_bit_register_is_sized():
    r = rows("atmega4809", {"ADC0.RES": 0x03FF})
    assert r["ADC0.RES"] == ("0x03FF", "-")
    assert rows("atmega4809")["VREF.CTRLA"][0] == "0x00"


def test_tiny2_adc_follows_its_own_header():
    r = rows("attiny3224", {"ADC0.CTRLB": 0x05, "ADC0.PGACTRL": 0x01, "ADC0.MUXPOS": 0x80})
    assert r["ADC0.CTRLB"][1] == "PRESC=DIV12", r["ADC0.CTRLB"]
    assert r["ADC0.PGACTRL"][1] == "PGAEN  ADCPGASAMPDUR=6CLK  PGABIASSEL=1X  GAIN=1X"
    assert r["ADC0.MUXPOS"][1] == "MUXPOS=0  VIA=2"             # reserved VIA value: a number
    assert "ADC0.SAMPLE" in r and "ADC0.MUXNEG" in r


def test_tiny1_vref_has_four_registers():
    assert [k for k in rows("attiny3217") if k.startswith("VREF")] == [f"VREF.CTRL{c}" for c in "ABCD"]


def test_vref_levels_use_datasheet_values():
    r = rows("atmega4809", {"VREF.CTRLA": 0x33})
    assert r["VREF.CTRLA"][1] == "AC0REFSEL=4V34 (4.3 V)  ADC0REFSEL=4V34 (4.3 V)"      # header name 4V34, DS 4.3 V
    assert rows("attiny3217", {"VREF.CTRLA": 0x21})["VREF.CTRLA"][1] == \
        "DAC0REFSEL=1V1 (1.1 V)  ADC0REFSEL=2V5 (2.5 V)"
    assert "1.024 V" in rows("attiny3224", {"VREF.CTRLA": 0x00})["VREF.CTRLA"][1]


def test_pads_come_from_the_device():
    r = rows("atmega4809", {"ADC0.MUXPOS": 0x0A, "AC0.MUXCTRLA": 0x02})
    assert r["ADC0.MUXPOS"][1] == "MUXPOS=AIN10 (PE2)"
    assert r["AC0.MUXCTRLA"][1] == "MUXNEG=PIN2 (PD7)  MUXPOS=PIN0 (PD2)"
    t = rows("attiny3217", {"ADC0.MUXPOS": 8, "ADC1.MUXPOS": 8})
    assert t["ADC0.MUXPOS"][1] == "MUXPOS=AIN8 (PB5)" and t["ADC1.MUXPOS"][1] == "MUXPOS=AIN8 (PC2)"
    # a pad the package lacks is said so, internal sources and unknown devices get nothing
    assert rows("attiny412", {"ADC0.MUXPOS": 4})["ADC0.MUXPOS"][1] == "MUXPOS=AIN4 (no pad on this device)"
    assert rows("attiny412", {"ADC0.MUXPOS": 3})["ADC0.MUXPOS"][1] == "MUXPOS=AIN3 (PA3)"
    assert rows("atmega4809", {"ADC0.MUXPOS": 0x1E})["ADC0.MUXPOS"][1] == "MUXPOS=TEMPSENSE"
    assert "(" not in rows("attiny3214", {"ADC0.MUXPOS": 3})["ADC0.MUXPOS"][1]      # no ATDF for 3214


def test_every_modern_core_decodes_with_pads():
    import json
    from pathlib import Path
    import simavr_py
    pads = json.loads((Path(simavr_py.__file__).parent / "analog_pins.json").read_text())
    assert len(pads) == 45
    for mmcu in pads:
        for b in analog(mmcu):
            decode(mmcu, b, lambda a, s: 0xA5, 3300)


def test_mega_adc_result_in_volts():
    mem = {"ADC0.CTRLC": 0x01 << 4 | 5, "ADC0.RES": 512}                    # REFSEL=VDDREF
    r = rows("atmega4809", mem, vdd_mv=5000)
    assert r["ADC0.Reference"][1] == "VDDREF  = 5 V  (VDD as set)"
    assert r["ADC0.Result"][1] == "RES = 512  ->  VIN ~ 2.502 V"            # 1023 * VIN / VREF
    # internal reference taken from VREF.ADC0REFSEL
    r = rows("atmega4809", {"ADC0.RES": 1023, "VREF.CTRLA": 0x20})
    assert r["ADC0.Reference"][1] == "2V5  = 2.5 V" and "VIN ~ 2.5 V" in r["ADC0.Result"][1]
    # accumulation divides by the sample count; 8-bit mode keeps the top eight bits
    r = rows("atmega4809", {"ADC0.CTRLB": 0x02, "ADC0.RES": 4 * 1023, "VREF.CTRLA": 0x20})
    assert "over 4 samples (avg 1023)" in r["ADC0.Result"][1] and "VIN ~ 2.5 V" in r["ADC0.Result"][1]
    r = rows("atmega4809", {"ADC0.CTRLA": 0x04, "ADC0.RES": 255, "VREF.CTRLA": 0x20})
    assert "VIN ~ 2.493 V" in r["ADC0.Result"][1] and "8-bit" in r["ADC0.Result"][1]


def test_external_reference_has_no_voltage():
    r = rows("atmega4809", {"ADC0.CTRLC": 0x20, "ADC0.RES": 300})
    assert r["ADC0.Reference"][1] == "VREFA  (external, voltage unknown)" and "ADC0.Result" not in r


def test_tiny1_second_adc_uses_its_own_vref_field():
    r = rows("attiny3217", {"ADC1.RES": 1023, "VREF.CTRLC": 0x20, "VREF.CTRLA": 0x00})
    assert r["ADC1.Reference"][1] == "2V5  = 2.5 V" and r["ADC0.Reference"][1] == "0V55  = 0.55 V"


def test_tiny2_sample_formats():
    base = {"ADC0.CTRLC": 6}                                                # 2500MV
    r = rows("attiny3224", {**base, "ADC0.COMMAND": 0x10, "ADC0.SAMPLE": 0x0800})
    assert r["ADC0.Result"][1] == "SAMPLE = 2048 of 4096  ->  VINP = 1.25 V"
    r = rows("attiny3224", {**base, "ADC0.COMMAND": 0x00, "ADC0.SAMPLE": 0x80})        # 8-bit: x256
    assert r["ADC0.Result"][1] == "SAMPLE = 128 of 256  ->  VINP = 1.25 V"
    r = rows("attiny3224", {**base, "ADC0.COMMAND": 0x90, "ADC0.SAMPLE": 0xF800})      # differential, -2048
    assert r["ADC0.Result"][1] == "SAMPLE = -2048 of 2048  ->  (VINP-VINN) = -2.5 V"
    r = rows("attiny3224", {**base, "ADC0.COMMAND": 0x10, "ADC0.CTRLF": 0x10, "ADC0.SAMPLE": 0x8000})
    assert "SAMPLE = 2048 of 4096" in r["ADC0.Result"][1]                    # LEFTADJ: code << 4
    pga = {"ADC0.PGACTRL": 0x41, "ADC0.MUXPOS": 0x40}                       # PGAEN, GAIN=4X, VIA=PGA
    r = rows("attiny3224", {**base, **pga, "ADC0.COMMAND": 0x10, "ADC0.SAMPLE": 0x0800})
    assert "VINP = 0.3125 V  (PGA 4x)" in r["ADC0.Result"][1]
    r = rows("attiny3224", {**base, "ADC0.PGACTRL": 0x41, "ADC0.COMMAND": 0x10, "ADC0.SAMPLE": 0x0800})
    assert "PGA" not in r["ADC0.Result"][1]                                 # PGA enabled but not selected (VIA)
    r = rows("attiny3224", {"ADC0.CTRLC": 4, "ADC0.COMMAND": 0x10, "ADC0.SAMPLE": 4095})
    assert r["ADC0.Reference"][1] == "1024MV  = 1.024 V"


def test_dacref_voltage():
    r = rows("atmega4809", {"AC0.DACREF": 128, "VREF.CTRLA": 0x02})
    assert r["AC0.DACREF voltage"][1] == "128 * 2V5 (2.5 V) / 256 = 1.25 V"
    r = rows("atmega4809", {"AC0.DACREF": 255, "VREF.CTRLA": 0x07}, vdd_mv=5000)
    assert "AVDD" in r["AC0.DACREF voltage"][1] and "4.98" in r["AC0.DACREF voltage"][1]
    assert not [k for k in rows("attiny3217") if k.endswith("DACREF voltage")]


def test_tiny1_temperature_from_sigrow():
    cal = {"SIGROW.TEMPSENSE0": 0xC0, "SIGROW.TEMPSENSE1": 0xF0}            # gain 192, offset -16
    base = {**cal, "ADC0.MUXPOS": 0x1E, "ADC0.CTRLC": 0x00, "VREF.CTRLA": 0x10, "ADC0.RES": 400}
    r = rows("attiny3217", base)
    # ((400 - -16) * 192 + 0x80) >> 8 = 312 K (DS40002205A p.434 user-code steps)
    assert r["ADC0.Temperature"][1] == "RES = 400, gain 192, offset -16  ->  312 K = 38.9 C"
    r = rows("attiny3217", {**base, "VREF.CTRLA": 0x20})                    # 2.5 V reference
    assert r["ADC0.Temperature"][1] == "needs the 1.1 V reference (selected: 2V5)"
    r = rows("attiny3217", {**base, "ADC0.CTRLB": 0x02})                    # accumulating
    assert "single-sample" in r["ADC0.Temperature"][1]
    r = rows("attiny3217", {**base, "ADC0.MUXPOS": 0x03})                   # a pin, not the sensor
    assert "ADC0.Temperature" not in r


def test_tiny1_dac_output():
    r = rows("attiny3217", {"DAC0.DATA": 128, "DAC0.CTRLA": 0x41, "VREF.CTRLA": 0x02})
    assert r["DAC0.Output"][1] == "128 * 2V5 (2.5 V) / 256 = 1.25 V  (nominal)"
    r = rows("attiny3217", {"DAC0.DATA": 255, "VREF.CTRLA": 0x01})
    assert r["DAC0.Output"][1] == "255 * 1V1 (1.1 V) / 256 = 1.096 V  (nominal, DAC disabled)"
    assert not [k for k in rows("atmega4809") if k.endswith("Output")]


SCRIPT = """
import sys
from PySide6.QtWidgets import QApplication
from simavr_py.gui.window import MainWindow

app = QApplication([])
win = MainWindow("atmega4809", 3333333, speed=0)
win.load(sys.argv[1])
win.run(); win.pause(); assert win._sim.wait_stopped(5)
win.refresh()
p = win.analogpane
rows = {(p._table.item(i, 0).text(), p._table.item(i, 1).text()): p._table.item(i, 3).text()
        for i in range(p._table.rowCount())}
assert rows["ADC0", "CTRLA"].startswith("ENABLE"), rows
assert "PRESC=DIV16" in rows["ADC0", "CTRLC"] and "REFSEL=VDDREF" in rows["ADC0", "CTRLC"], rows
assert rows["ADC0", "MUXPOS"] == "MUXPOS=AIN3 (PD3)", rows
assert rows["ADC0", "Reference"] == "VDDREF  = 3.3 V  (VDD as set)", rows
assert rows["ADC0", "Result"].startswith("RES = 0  ->  VIN ~ 0 V"), rows
assert rows["AC0", "MUXCTRLA"] == "MUXNEG=PIN1 (PD5)  MUXPOS=PIN2 (PD6)", rows
win.close()
"""


def test_analog_pane(tmp_path):
    src = tmp_path / "t.c"
    src.write_text("#include <avr/io.h>\nint main(void){"
                   "ADC0.CTRLC=ADC_PRESC_DIV16_gc|ADC_REFSEL_VDDREF_gc;ADC0.MUXPOS=ADC_MUXPOS_AIN3_gc;"
                   "ADC0.CTRLA=ADC_ENABLE_bm;"
                   "AC0.MUXCTRLA=AC_MUXPOS_PIN2_gc|AC_MUXNEG_PIN1_gc;for(;;);}\n")
    elf = tmp_path / "t.elf"
    r = subprocess.run(["avr-gcc", "-mmcu=atmega4809", "-Os", "-o", elf, src],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    env = {**os.environ, "QT_QPA_PLATFORM": "offscreen"}
    r = subprocess.run([sys.executable, "-c", SCRIPT, str(elf)], env=env, capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
