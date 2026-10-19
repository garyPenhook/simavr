"""Declarative board definitions: a board is data (JSON or TOML) naming the
virtual parts wired to MCU pins, not glue code.

    {"name": "demo", "mmcu": "atmega4809", "freq": 3333333,
     "widgets": [
       {"type": "led",     "pin": "PA0", "label": "LED", "color": "red"},
       {"type": "button",  "pin": "PB2", "label": "SW1", "active_low": true},
       {"type": "encoder", "pin_a": "PC0", "pin_b": "PC1", "label": "ENC"}]}
"""

import json
import tomllib
from dataclasses import dataclass, field
from pathlib import Path

from .widgets import ButtonModel, EncoderModel, LedModel, Write


class BoardError(ValueError):
    """A board file that can't be used."""


@dataclass(frozen=True, slots=True)
class LedSpec:
    pin: str
    label: str = ""
    color: str = "red"
    active_low: bool = False


@dataclass(frozen=True, slots=True)
class ButtonSpec:
    pin: str
    label: str = ""
    active_low: bool = True
    latch: bool = False
    pull: bool = False


@dataclass(frozen=True, slots=True)
class EncoderSpec:
    pin_a: str
    pin_b: str
    label: str = ""
    active_low: bool = True


type WidgetSpec = LedSpec | ButtonSpec | EncoderSpec


@dataclass(frozen=True, slots=True)
class Board:
    name: str = "board"
    mmcu: str | None = None
    freq: int = 0
    widgets: tuple[WidgetSpec, ...] = ()

    def pins(self) -> list[str]:
        out = []
        for w in self.widgets:
            match w:
                case LedSpec(pin=p) | ButtonSpec(pin=p):
                    out.append(p)
                case EncoderSpec(pin_a=a, pin_b=b):
                    out += [a, b]
        return out

    def missing_pins(self, available) -> list[str]:
        """Wired pins the MCU doesn't have (e.g. PORTC pins on a small part)."""
        return sorted({p for p in self.pins() if p not in available})


_KINDS: dict[str, tuple[type, dict[str, type]]] = {
    "led": (LedSpec, {"pin": str, "label": str, "color": str, "active_low": bool}),
    "button": (ButtonSpec, {"pin": str, "label": str, "active_low": bool,
                            "latch": bool, "pull": bool}),
    "encoder": (EncoderSpec, {"pin_a": str, "pin_b": str, "label": str,
                              "active_low": bool}),
}
_PIN_FIELDS = {"pin", "pin_a", "pin_b"}


def _widget(d, index: int) -> WidgetSpec:
    where = f"widgets[{index}]"
    if not isinstance(d, dict):
        raise BoardError(f"{where}: expected an object")
    d = dict(d)
    kind = d.pop("type", None)
    if kind not in _KINDS:
        raise BoardError(f"{where}: unknown type {kind!r} (want one of {', '.join(_KINDS)})")
    cls, fields = _KINDS[kind]
    for key, value in d.items():
        if key not in fields:
            raise BoardError(f"{where}: unknown key {key!r} for a {kind}")
        if not isinstance(value, fields[key]):
            raise BoardError(f"{where}: {key!r} must be {fields[key].__name__}")
        if key in _PIN_FIELDS:
            d[key] = value.strip().upper()
    required = [f for f in fields if f.startswith("pin")]
    if missing := [f for f in required if f not in d]:
        raise BoardError(f"{where}: a {kind} needs {', '.join(missing)}")
    return cls(**d)


def board_from_dict(d) -> Board:
    if not isinstance(d, dict):
        raise BoardError("board must be an object")
    unknown = set(d) - {"name", "mmcu", "freq", "widgets"}
    if unknown:
        raise BoardError(f"unknown board key(s): {', '.join(sorted(unknown))}")
    widgets = d.get("widgets", [])
    if not isinstance(widgets, list):
        raise BoardError("'widgets' must be a list")
    freq = d.get("freq", 0)
    if not isinstance(freq, int) or isinstance(freq, bool) or freq < 0:
        raise BoardError("'freq' must be a non-negative integer")
    for key in ("name", "mmcu"):
        if key in d and not isinstance(d[key], str):
            raise BoardError(f"{key!r} must be a string")
    board = Board(name=d.get("name", "board"), mmcu=d.get("mmcu"), freq=freq,
                  widgets=tuple(_widget(w, i) for i, w in enumerate(widgets)))
    pins = board.pins()
    if dup := sorted({p for p in pins if pins.count(p) > 1}):
        raise BoardError(f"pin(s) wired more than once: {', '.join(dup)}")
    return board


def load_board(path: str | Path) -> Board:
    path = Path(path)
    try:
        if path.suffix.lower() == ".toml":
            with open(path, "rb") as f:
                return board_from_dict(tomllib.load(f))
        with open(path) as f:
            return board_from_dict(json.load(f))
    except (OSError, json.JSONDecodeError, tomllib.TOMLDecodeError) as e:
        raise BoardError(f"{path}: {e}") from e


@dataclass(slots=True)
class BoardRuntime:
    """A board bound to one simulator: owns the widget models and the pin
    subscriptions, routes events in and widget input out. Not thread-safe;
    use from the GUI thread like the rest of the shell."""

    board: Board
    sim: object                 # Simulator (not imported: keep this module cheap)
    leds: dict[int, LedModel] = field(default_factory=dict)   # widget index
    buttons: dict[int, ButtonModel] = field(default_factory=dict)
    encoders: dict[int, EncoderModel] = field(default_factory=dict)
    _by_pin: dict[int, list[LedModel]] = field(default_factory=dict)
    _subs: list[int] = field(default_factory=list)

    # An LED edge closer than this to the last is folded into it by the host;
    # ~0.5 ms of simulated time keeps PWM duty readable without flooding.
    LED_COALESCE_DIV = 2000

    def __post_init__(self):
        sim = self.sim
        if missing := self.board.missing_pins(sim.pins):
            raise BoardError(f"{sim.mmcu} has no pin(s) {', '.join(missing)}")
        try:
            self._bind()
        except BaseException:
            self.close()            # don't leak subscriptions / held pins
            raise

    def _bind(self):
        sim = self.sim
        coalesce = max(1, (sim.frequency or 1_000_000) // self.LED_COALESCE_DIV)
        now = sim.cycle
        for i, w in enumerate(self.board.widgets):
            match w:
                case LedSpec(pin=p, active_low=al):
                    m = LedModel(active_low=al)
                    m.reset(now, sim.pin_state(p).level)
                    self.leds[i] = m
                    self._by_pin.setdefault(sim.pin(p), []).append(m)
                    self._subs.append(sim.subscribe_pin(p, coalesce))
                case ButtonSpec(pin=p, active_low=al, latch=lt, pull=pu):
                    self.buttons[i] = m = ButtonModel(p, al, lt, pu)
                    self.apply(m.idle())
                case EncoderSpec(pin_a=a, pin_b=b, active_low=al):
                    self.encoders[i] = EncoderModel(a, b, al)

    def dispatch(self, ev) -> None:
        """Feed one event; only PinEvents for a wired LED pin matter."""
        for m in self._by_pin.get(getattr(ev, "pin", -1), ()):
            m.on_pin(ev.cycle, ev.level)

    def brightness(self, index: int, cycle: int | None = None) -> float:
        """LED brightness; pass one `cycle` to sample every LED at the same instant."""
        return self.leds[index].sample(self.sim.cycle if cycle is None else cycle)

    def apply(self, write: Write | None) -> None:
        if write is None:
            return
        pin, level = write
        if level is None:
            self.sim.release(pin)
        else:
            self.sim.drive(pin, level)

    def close(self) -> None:
        """Unsubscribe and let go of every pin this board was holding."""
        subs, self._subs = self._subs, []
        for s in subs:
            try:
                self.sim.unsubscribe(s)
            except Exception:       # sim already closed
                pass
        for m in self.buttons.values():
            try:
                self.sim.release(m.pin)
            except Exception:
                pass
        for e in self.encoders.values():
            for p in (e.pin_a, e.pin_b):
                try:
                    self.sim.release(p)
                except Exception:
                    pass
