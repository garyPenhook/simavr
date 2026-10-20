"""SSD1306: renderer and board spec (no Qt, no firmware), then the native part against the
real board_atmega4809_oled firmware over I2C and SPI, and the board widget in the GUI."""

import os
import shutil
import subprocess
import sys
from pathlib import Path
from types import SimpleNamespace

import pytest

import simavr_py as sa
from simavr_py.boards import BoardError, OledSpec, board_from_dict
from simavr_py.widgets import OLED_H, OLED_W, oled_pixels

REPO = Path(__file__).resolve().parents[2]
FW = REPO / "examples" / "board_atmega4809_oled"


def state(**kw) -> SimpleNamespace:
    d = dict(seq=1, on=True, all_on=False, inverse=False, remap=False, com_flip=False,
             contrast=0x7F, start_line=0, addr_mode=2, vram=bytes(1024))
    d.update(kw)
    return SimpleNamespace(**d)


def px(pix, x, y):
    return pix[y * OLED_W + x]


def vram_with(page, col, value):
    v = bytearray(1024)
    v[page * 128 + col] = value
    return bytes(v)


# ---------------------------------------------------------------- renderer


def test_render_basic_bit_order():
    pix = oled_pixels(state(vram=vram_with(2, 5, 0b0000_0101)))
    assert [px(pix, 5, 16 + b) for b in range(8)] == [1, 0, 1, 0, 0, 0, 0, 0]   # bit 0 on top
    assert sum(pix) == 2


def test_render_display_off_and_all_on():
    v = vram_with(0, 0, 0xFF)
    assert sum(oled_pixels(state(on=False, vram=v))) == 0
    assert sum(oled_pixels(state(all_on=True, vram=bytes(1024)))) == OLED_W * OLED_H


def test_render_inverse():
    pix = oled_pixels(state(inverse=True, vram=vram_with(0, 0, 0x01)))
    assert px(pix, 0, 0) == 0 and sum(pix) == OLED_W * OLED_H - 1


def test_render_remap_and_com_flip():
    v = vram_with(0, 0, 0x01)                    # RAM column 0, row 0
    assert px(oled_pixels(state(vram=v)), 0, 0) == 1
    assert px(oled_pixels(state(vram=v, remap=True)), 127, 0) == 1           # A1h
    assert px(oled_pixels(state(vram=v, com_flip=True)), 0, 63) == 1         # C8h
    assert px(oled_pixels(state(vram=v, remap=True, com_flip=True)), 127, 63) == 1


def test_render_start_line_scrolls_rows():
    v = vram_with(1, 0, 0x01)                    # RAM row 8
    assert px(oled_pixels(state(vram=v, start_line=8)), 0, 0) == 1           # row 8 on COM0
    assert px(oled_pixels(state(vram=v, start_line=9)), 0, 63) == 1          # wraps around


# ---------------------------------------------------------------- board spec


def test_board_oled_i2c_and_spi():
    b = board_from_dict({"widgets": [
        {"type": "oled"},
        {"type": "oled", "bus": "SPI", "dc": "pc1", "cs": "pc0", "res": "pc2", "instance": 0}]})
    assert b.widgets[0] == OledSpec()
    assert b.widgets[1].bus == "spi" and b.widgets[1].dc == "PC1"
    assert b.pins() == ["PC0", "PC1", "PC2"]


@pytest.mark.parametrize("w, msg", [
    ({"type": "oled", "bus": "uart"}, "'bus'"),
    ({"type": "oled", "addr": 0x50}, "0x3C"),
    ({"type": "oled", "dc": "PC1"}, "'cs'/'dc'"),
    ({"type": "oled", "bus": "spi"}, "needs 'dc'"),
    ({"type": "oled", "bus": "spi", "dc": "PC1", "addr": 60}, "'addr'"),
    ({"type": "oled", "instance": 12}, "'instance'"),
])
def test_board_oled_rejects(w, msg):
    with pytest.raises(BoardError, match=msg):
        board_from_dict({"widgets": [w]})


# ---------------------------------------------------------------- firmware


def _build(tmp_path, name):
    if not shutil.which("avr-gcc"):
        pytest.skip("no avr-gcc")
    elf = tmp_path / f"{name}.elf"
    r = subprocess.run(["avr-gcc", "-mmcu=atmega4809", "-Os", "-DF_CPU=3333333UL",
                        "-I", str(REPO / "simavr" / "sim" / "avr"), "-o", elf, FW / f"{name}.c"],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    return elf


def _lit(st):
    return sum(bin(b).count("1") for b in st.vram)


def test_i2c_firmware_draws_the_pattern(tmp_path):
    elf = _build(tmp_path, "atmega4809_oled_i2c")
    with sa.Simulator.from_elf(elf, "atmega4809") as s:
        oled = s.attach_oled("twi", 0, 0x3C)
        before = s.read_oled(oled)
        assert not before.on and _lit(before) == 0 and before.addr_mode == 2   # reset state
        s.run_cycles(3333333 // 3)                                              # ~330 ms
        assert s.wait_stopped(30)
        st = s.read_oled(oled)
    assert st.seq != before.seq
    assert st.on and st.remap and st.com_flip and st.contrast == 0x7F
    assert st.addr_mode == 0                          # the firmware selects horizontal
    assert _lit(st) == 4160                           # same count as the C example's "check"
    pix = oled_pixels(st)
    assert sum(pix) == 4160


def test_i2c_wrong_address_is_ignored(tmp_path):
    elf = _build(tmp_path, "atmega4809_oled_i2c")
    with sa.Simulator.from_elf(elf, "atmega4809") as s:
        oled = s.attach_oled("twi", 0, 0x3D)            # firmware talks to 0x3C
        s.run_cycles(3333333 // 10)
        assert s.wait_stopped(30)
        st = s.read_oled(oled)
    assert not st.on and _lit(st) == 0


def test_spi_firmware_draws_the_pattern(tmp_path):
    elf = _build(tmp_path, "atmega4809_oled_spi")
    with sa.Simulator.from_elf(elf, "atmega4809") as s:
        oled = s.attach_oled("spi", 0, cs="PC0", dc="PC1", res="PC2")
        s.run_cycles(3333333 // 3)
        assert s.wait_stopped(30)
        st = s.read_oled(oled)
    assert st.on and _lit(st) == 4160


def test_attach_errors_and_detach():
    with sa.Simulator("atmega4809") as s:
        with pytest.raises(sa.InvalidArgument):
            s.attach_oled("spi", 0)                      # SPI needs D/C
        with pytest.raises(sa.InvalidArgument):
            s.attach_oled("twi", 0, 0x50)
        with pytest.raises(sa.NotFound):
            s.attach_oled("twi", 3)                      # no TWI3
        with pytest.raises(sa.NotFound):
            s.attach_oled("twi", 0, res="PZ9")
        ids = [s.attach_oled("twi", 0) for _ in range(4)]
        assert ids == [0, 1, 2, 3]
        with pytest.raises(sa.OutOfRange):
            s.attach_oled("twi", 0)
        s.detach_oled(2)
        assert s.attach_oled("twi", 0) == 2              # slot reused
        with pytest.raises(sa.NotFound):
            s.read_oled(9)
        s.detach_oled(0)
        with pytest.raises(sa.NotFound):
            s.read_oled(0)


# ---------------------------------------------------------------- GUI widget


GUI_SCRIPT = """
import sys, time
from PySide6.QtWidgets import QApplication
from simavr_py.boards import load_board
from simavr_py.gui.window import MainWindow

app = QApplication([])
win = MainWindow("atmega4809", 3333333, speed=0)
win.load_board(sys.argv[2])
win.load(sys.argv[1])
assert len(win.board._oleds) == 1
oled = next(iter(win.board._oleds.values()))
win.run()
t = time.time()
while time.time() - t < 15:
    app.processEvents(); win._drain(); win.refresh()
    if oled._img is not None and oled._seen > 100:
        break
    time.sleep(0.01)
win.pause(); assert win._sim.wait_stopped(5)
win.refresh()
assert oled._img is not None
lit = sum(1 for y in range(64) for x in range(128) if oled._img.pixelIndex(x, y) == 1)
assert lit > 1000, lit
# reloading the board on the same simulator must not leak OLED slots
for _ in range(6):
    win.load_board(sys.argv[2])
win.close()
"""


def test_oled_widget_in_gui(tmp_path):
    elf = _build(tmp_path, "atmega4809_oled_i2c")
    env = {**os.environ, "QT_QPA_PLATFORM": "offscreen"}
    board = REPO / "python" / "boards" / "oled_4809.json"
    r = subprocess.run([sys.executable, "-c", GUI_SCRIPT, str(elf), str(board)], env=env,
                       capture_output=True, text=True, timeout=120)
    assert r.returncode == 0, r.stdout + r.stderr
