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
