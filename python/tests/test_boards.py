"""Board definitions and widget models: pure logic, no Qt and no simulator."""

import json

import pytest

from simavr_py.boards import (
    BoardError, ButtonSpec, EncoderSpec, LedSpec, board_from_dict, load_board,
)
from simavr_py.widgets import ButtonModel, EncoderModel, LedModel


def test_parse_and_normalise():
    b = board_from_dict({"name": "x", "mmcu": "atmega4809", "freq": 1000, "widgets": [
        {"type": "led", "pin": " pa0 ", "color": "blue"},
        {"type": "button", "pin": "PB2", "latch": True},
        {"type": "encoder", "pin_a": "pc0", "pin_b": "PC1"},
    ]})
    assert b.widgets[0] == LedSpec("PA0", color="blue")
    assert b.widgets[1] == ButtonSpec("PB2", latch=True)
    assert b.widgets[2] == EncoderSpec("PC0", "PC1")
    assert b.pins() == ["PA0", "PB2", "PC0", "PC1"]
    assert b.missing_pins({"PA0", "PB2", "PC0"}) == ["PC1"]


@pytest.mark.parametrize("bad", [
    [],
    {"bogus": 1},
    {"freq": -1},
    {"freq": True},
    {"widgets": {}},
    {"widgets": [3]},
    {"widgets": [{"type": "knob"}]},
    {"widgets": [{"type": "led"}]},                       # no pin
    {"widgets": [{"type": "led", "pin": "PA0", "x": 1}]},
    {"widgets": [{"type": "led", "pin": 3}]},
    {"widgets": [{"type": "button", "pin": "PA0", "latch": "yes"}]},
])
def test_rejects_bad_boards(bad):
    with pytest.raises(BoardError):
        board_from_dict(bad)


def test_load_json_toml_and_errors(tmp_path):
    j = tmp_path / "a.json"
    j.write_text(json.dumps({"widgets": [{"type": "led", "pin": "PA0"}]}))
    t = tmp_path / "a.toml"
    t.write_text('[[widgets]]\ntype = "led"\npin = "PA0"\n')
    assert load_board(j).widgets == load_board(t).widgets
    (tmp_path / "bad.json").write_text("{")
    for p in (tmp_path / "bad.json", tmp_path / "missing.json"):
        with pytest.raises(BoardError):
            load_board(p)


def test_shipped_boards_parse():
    from pathlib import Path
    for p in (Path(__file__).parent.parent / "boards").glob("*.json"):
        assert load_board(p).widgets


def test_led_duty_and_polarity():
    led = LedModel()
    led.reset(0, False)
    led.on_pin(100, True)           # on for half of 0..200
    assert led.sample(200) == pytest.approx(0.5)
    assert led.sample(300) == 1.0   # stays lit
    low = LedModel(active_low=True)
    low.reset(0, True)              # high = off
    assert low.sample(100) == 0.0
    low.on_pin(100, False)
    assert low.sample(200) == 1.0


def test_led_paused_keeps_value_and_survives_reset():
    led = LedModel()
    led.reset(1000, True)
    assert led.sample(1000) == 1.0      # no time passed: previous stands
    led.on_pin(500, False)              # cycle went backwards (sim reset)
    assert led.sample(600) == 0.0


def test_button_momentary_latch_and_pull():
    b = ButtonModel("PB2")
    assert b.idle() is None
    assert b.press() == ("PB2", False)      # active low: pressed pulls low
    assert b.press() is None                # repeat press is a no-op
    assert b.release() == ("PB2", None)     # released: pin let go
    assert b.release() is None
    p = ButtonModel("PB2", pull=True)
    assert p.idle() == ("PB2", True)
    assert p.release() is None and p.press() == ("PB2", False)
    assert p.release() == ("PB2", True)
    s = ButtonModel("PB3", latch=True, active_low=False)
    assert s.press() == ("PB3", True)
    assert s.release() is None
    assert s.press() == ("PB3", None)       # second press switches it off


def test_encoder_quadrature():
    e = EncoderModel("PC0", "PC1")
    cw = e.turn(+1)
    assert len(cw) == 4 and e.phase == 0
    # A closes first when turning clockwise, and exactly one contact changes per step.
    assert cw[0] == [("PC0", False), ("PC1", None)]
    assert cw[1] == [("PC0", False), ("PC1", False)]
    ccw = e.turn(-1)
    assert ccw[0] == [("PC0", None), ("PC1", False)]     # B closes first
    assert e.phase == 0


def test_rejects_shared_pins():
    for widgets in ([{"type": "button", "pin": "PB2"}, {"type": "led", "pin": "PB2"}],
                    [{"type": "encoder", "pin_a": "PC0", "pin_b": "PC0"}]):
        with pytest.raises(BoardError):
            board_from_dict({"widgets": widgets})


def test_led_reset_by_cycle_regression_clears_state():
    led = LedModel()
    led.reset(0, True)
    assert led.sample(1000) == 1.0
    led.sample(10)                          # regression: state reset, no span yet
    assert led.sample(110) == 0.0           # low after reset, no edge since


# -- HD44780 ------------------------------------------------------------------

def _send(lcd, byte, rs, four=True):
    """Drive one byte the way firmware does: RS, data, then E high -> low."""
    def nibble(n, base):
        lcd.on_pin("rs", rs)
        for i in range(4):
            lcd.on_pin(f"d{base + i}", bool(n >> i & 1))
        lcd.on_pin("e", True)
        lcd.on_pin("e", False)
    if four:
        nibble(byte >> 4, 4)
        nibble(byte & 0xF, 4)
    else:
        lcd.on_pin("rs", rs)
        for i in range(8):
            lcd.on_pin(f"d{i}", bool(byte >> i & 1))
        lcd.on_pin("e", True)
        lcd.on_pin("e", False)


def _init4(lcd):
    """The datasheet's 4-bit start-up: three 0x3 nibbles, 0x2, then function set."""
    for _ in range(3):
        for i, v in enumerate((1, 1, 0, 0)):        # only the high nibble is wired
            lcd.on_pin(f"d{4 + i}", bool(v))
        lcd.on_pin("e", True)
        lcd.on_pin("e", False)
    for i, v in enumerate((0, 1, 0, 0)):            # 0x2: switch to 4-bit
        lcd.on_pin(f"d{4 + i}", bool(v))
    lcd.on_pin("e", True)
    lcd.on_pin("e", False)
    assert not lcd.eight_bit and lcd._high is None
    _send(lcd, 0x28, False)                         # 4-bit, 2 lines
    _send(lcd, 0x0C, False)                         # display on
    _send(lcd, 0x06, False)                         # entry mode: increment
    _send(lcd, 0x01, False)                         # clear


def _text(lcd, s):
    for ch in s:
        _send(lcd, ord(ch), True)


def test_lcd_4bit_init_and_text():
    from simavr_py.widgets import LcdModel
    lcd = LcdModel()
    _init4(lcd)
    assert lcd.two_line and lcd.display_on and lcd.lines() == [" " * 16] * 2
    _text(lcd, "Hello")
    _send(lcd, 0x80 | 0x40, False)                  # DDRAM 0x40: second line
    _text(lcd, "World")
    assert lcd.lines() == ["Hello".ljust(16), "World".ljust(16)]
    assert lcd.cursor_pos() == (1, 5)
    v = lcd.version
    lcd.on_pin("e", True)                           # rising edge alone latches nothing
    assert lcd.version == v


def test_lcd_8bit_and_display_off():
    from simavr_py.widgets import LcdModel
    lcd = LcdModel(cols=8, rows=1)
    _send(lcd, 0x38, False, four=False)
    _send(lcd, 0x0F, False, four=False)             # on, cursor, blink
    _text_8 = lambda s: [_send(lcd, ord(c), True, four=False) for c in s]
    _text_8("ABC")
    assert lcd.eight_bit and lcd.lines() == ["ABC     "]
    assert lcd.cursor_on and lcd.blink_on and lcd.cursor_pos() == (0, 3)
    _send(lcd, 0x08, False, four=False)             # display off keeps DDRAM
    assert lcd.lines() == [" " * 8] and lcd.cursor_pos() is None
    _send(lcd, 0x0C, False, four=False)
    assert lcd.lines() == ["ABC     "]


def test_lcd_ignores_reads_and_half_bytes():
    from simavr_py.widgets import LcdModel
    lcd = LcdModel()
    _init4(lcd)
    lcd.on_pin("rw", True)
    _text(lcd, "X")                                 # RW high: not a write
    assert lcd.lines()[0].strip() == ""
    lcd.on_pin("rw", False)


def test_lcd_cgram_shift_and_cursor_moves():
    from simavr_py.widgets import LcdModel
    lcd = LcdModel()
    _init4(lcd)
    _send(lcd, 0x40, False)                         # CGRAM 0
    for row in (0x04, 0x0E, 0x1F, 0x1F, 0x1F, 0x0E, 0x04, 0x00):
        _send(lcd, row, True)
    assert lcd.glyph_rows(0) == [0x04, 0x0E, 0x1F, 0x1F, 0x1F, 0x0E, 0x04, 0x00]
    assert lcd.glyph_rows(8) == lcd.glyph_rows(0)   # codes 8..15 mirror 0..7
    _send(lcd, 0x80, False)                         # back to DDRAM
    _text(lcd, "ab")
    _send(lcd, 0x18, False)                         # shift display left
    assert lcd.lines()[0].startswith("b ")
    _send(lcd, 0x1C, False)                         # and right again
    assert lcd.lines()[0].startswith("ab")
    _send(lcd, 0x10, False)                         # cursor left
    assert lcd.cursor_pos() == (0, 1)
    _send(lcd, 0x02, False)                         # home
    assert lcd.cursor_pos() == (0, 0) and lcd.shift == 0


def test_lcd_entry_shift_keeps_cursor_in_place():
    from simavr_py.widgets import LcdModel
    lcd = LcdModel()
    _init4(lcd)
    _send(lcd, 0x07, False)                         # increment + display shift
    _text(lcd, "ab")
    assert lcd.shift == 2 and lcd.cursor_pos() == (0, 0)


def test_lcd_clear_resets_lost_flag():
    from simavr_py.widgets import LcdModel
    lcd = LcdModel()
    _init4(lcd)
    lcd.lost = True
    _send(lcd, 0x01, False)
    assert not lcd.lost


def test_lcd_board_spec():
    b = board_from_dict({"widgets": [
        {"type": "lcd", "rs": "pd0", "e": "PD1", "rw": "PD2",
         "data": ["pd4", "PD5", "PD6", "PD7"], "rows": 4, "cols": 20}]})
    w = b.widgets[0]
    assert w.data == ("PD4", "PD5", "PD6", "PD7") and (w.cols, w.rows) == (20, 4)
    assert w.roles() == {"PD0": "rs", "PD1": "e", "PD2": "rw",
                         "PD4": "d4", "PD5": "d5", "PD6": "d6", "PD7": "d7"}
    assert b.pins() == ["PD0", "PD1", "PD2", "PD4", "PD5", "PD6", "PD7"]
    eight = board_from_dict({"widgets": [{"type": "lcd", "rs": "PD0", "e": "PD1",
                                          "data": [f"PC{i}" for i in range(8)]}]})
    assert eight.widgets[0].roles()["PC0"] == "d0"


@pytest.mark.parametrize("bad", [
    {"rs": "PD0", "e": "PD1"},                                  # no data
    {"rs": "PD0", "data": ["PD4"] * 4},                         # no e
    {"rs": "PD0", "e": "PD1", "data": ["PD4", "PD5"]},          # wrong width
    {"rs": "PD0", "e": "PD1", "data": [4, 5, 6, 7]},
    {"rs": "PD0", "e": "PD1", "data": ["PD4", "PD5", "PD6", "PD7"], "rows": 3},
    {"rs": "PD0", "e": "PD1", "data": ["PD4", "PD5", "PD6", "PD7"], "cols": 0},
    {"rs": "PD0", "e": "PD1", "data": ["PD4", "PD5", "PD6", "PD7"], "cols": True},
    {"rs": "PD0", "e": "PD0", "data": ["PD4", "PD5", "PD6", "PD7"]},   # shared pin
    {"rs": "PD0", "e": "PD1", "data": ["PD4", "PD5", "PD6", "PD4"]},   # shared pin
])
def test_lcd_rejects_bad_specs(bad):
    with pytest.raises(BoardError):
        board_from_dict({"widgets": [{"type": "lcd", **bad}]})


def test_lcd_resync_drops_half_byte_and_adopts_levels():
    from simavr_py.widgets import LcdModel
    lcd = LcdModel()
    _init4(lcd)
    lcd.on_pin("d4", True)
    lcd.on_pin("e", True)
    lcd.on_pin("e", False)                  # high nibble latched
    assert lcd._high is not None
    lcd.resync({"e": False, "rs": True})
    assert lcd._high is None and lcd.lost
    _text(lcd, "Z")                         # pairing is back in step
    assert lcd.lines()[0].startswith("Z")


def test_placement_parses_and_validates():
    b = board_from_dict({"widgets": [
        {"type": "led", "pin": "PA0", "row": 2, "col": 3},
        {"type": "led", "pin": "PA1"},
    ]})
    assert (b.widgets[0].row, b.widgets[0].col) == (2, 3)
    assert (b.widgets[1].row, b.widgets[1].col) == (-1, -1)
    for bad in (
        [{"type": "led", "pin": "PA0", "row": 1}],                    # half a cell
        [{"type": "led", "pin": "PA0", "row": -1, "col": 0}],
        [{"type": "led", "pin": "PA0", "row": True, "col": 0}],
        [{"type": "led", "pin": "PA0", "row": 0, "col": 0},
         {"type": "led", "pin": "PA1", "row": 0, "col": 0}],           # same cell
    ):
        with pytest.raises(BoardError):
            board_from_dict({"widgets": bad})


def test_4809_nano_demo_runs_on_its_board():
    """examples/demo_4809_nano.elf on boards/curiosity_nano_4809.json: LED0 (PF5, active low)
    blinks from the TCA0 overflow ISR, is held on while SW0 (PF6) is pressed, and USART0 (PA0)
    prints a line per tick. Pins per DS40002173C 4.1 (USART0 TxD on PA0 by default)."""
    from pathlib import Path
    import simavr_py.core as sa
    here = Path(__file__).resolve().parent.parent
    board = load_board(here / "boards" / "curiosity_nano_4809.json")
    sim = sa.Simulator(board.mmcu, board.freq)
    sim.load_elf(here / "examples" / "demo_4809_nano.elf")
    sim.reset()
    sim.subscribe_uart(0)
    sim.run_cycles(board.freq // 10)
    assert sim.pin_state("PF5").dir and sim.pin_state("PF6").pullup          # LED output, SW0 pulled up
    seen = set()
    for _ in range(40):
        sim.run_cycles(board.freq // 20)
        seen.add(sim.pin_state("PF5").out)
    assert seen == {True, False}                                              # blinking
    sim.events()
    sim.drive("PF6", False)                                                   # press SW0
    sim.run_cycles(board.freq // 2)
    assert not sim.pin_state("PF5").out                                       # held on (active low)
    sim.run_cycles(board.freq)
    text = bytes(e.byte for e in sim.events() if isinstance(e, sa.UartTxEvent)).decode()
    assert "sw=0" in text and "tick " in text
