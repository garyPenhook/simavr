"""Declarative board definitions: a board is data (JSON or TOML) naming the
virtual parts wired to MCU pins, not glue code.

    {"name": "demo", "mmcu": "atmega4809", "freq": 3333333,
     "widgets": [
       {"type": "led",     "pin": "PA0", "label": "LED", "color": "red"},
       {"type": "button",  "pin": "PB2", "label": "SW1", "active_low": true},
       {"type": "encoder", "pin_a": "PC0", "pin_b": "PC1", "label": "ENC"},
       {"type": "lcd", "rs": "PD0", "e": "PD1", "data": ["PD4", "PD5", "PD6", "PD7"],
        "cols": 16, "rows": 2}]}

An HD44780 `lcd` lists `data` as D4..D7 (4-bit) or D0..D7 (8-bit); `rw` is
optional and, when wired, a pulse with RW high is ignored (writes only).
"""

import json
import tomllib
from dataclasses import dataclass, field
from pathlib import Path

lazy from .core import OverflowEvent
from .widgets import ButtonModel, EncoderModel, LcdModel, LedModel, Write


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


@dataclass(frozen=True, slots=True)
class LcdSpec:
    rs: str
    e: str
    data: tuple[str, ...]           # D4..D7 or D0..D7
    rw: str = ""
    cols: int = 16
    rows: int = 2
    label: str = ""

    def roles(self) -> dict[str, str]:
        """MCU pin -> LCD signal name, as `LcdModel.on_pin` takes it."""
        first = 8 - len(self.data)
        out = {self.rs: "rs", self.e: "e"}
        if self.rw:
            out[self.rw] = "rw"
        out.update({p: f"d{first + i}" for i, p in enumerate(self.data)})
        return out


@dataclass(frozen=True, slots=True)
class OledSpec:
    """128x64 SSD1306 on TWI<instance> (`addr` 0x3C/0x3D) or 4-wire SPI<instance>. The
    bus pins belong to the firmware; `cs`/`dc`/`res` are the GPIOs the panel listens to
    (SPI needs `dc`; I2C only ever uses `res`)."""
    bus: str = "twi"
    instance: int = 0
    addr: int = 0x3C
    cs: str = ""
    dc: str = ""
    res: str = ""
    label: str = ""


type WidgetSpec = LedSpec | ButtonSpec | EncoderSpec | LcdSpec | OledSpec


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
                case LcdSpec(rs=rs, e=e, rw=rw, data=data):
                    out += [rs, e, *([rw] if rw else []), *data]   # raw: dups must show
                case OledSpec(cs=cs, dc=dc, res=res):
                    out += [p for p in (cs, dc, res) if p]
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
    "lcd": (LcdSpec, {"rs": str, "e": str, "rw": str, "data": list,
                      "cols": int, "rows": int, "label": str}),
    "oled": (OledSpec, {"bus": str, "instance": int, "addr": int, "cs": str, "dc": str,
                        "res": str, "label": str}),
}
_PIN_FIELDS = {"pin", "pin_a", "pin_b", "rs", "e", "rw", "cs", "dc", "res"}
_REQUIRED = {"led": ("pin",), "button": ("pin",), "encoder": ("pin_a", "pin_b"),
             "lcd": ("rs", "e", "data"), "oled": ()}


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
        if not isinstance(value, fields[key]) or (
                fields[key] is int and isinstance(value, bool)):
            raise BoardError(f"{where}: {key!r} must be {fields[key].__name__}")
        if key in _PIN_FIELDS:
            d[key] = value.strip().upper()
    if missing := [f for f in _REQUIRED[kind] if f not in d]:
        raise BoardError(f"{where}: a {kind} needs {', '.join(missing)}")
    if kind == "lcd":
        _check_lcd(d, where)
    if kind == "oled":
        _check_oled(d, where)
    return cls(**d)


def _check_lcd(d: dict, where: str) -> None:
    data = d["data"]
    if len(data) not in (4, 8) or not all(isinstance(p, str) for p in data):
        raise BoardError(f"{where}: 'data' must list 4 (D4..D7) or 8 (D0..D7) pins")
    d["data"] = tuple(p.strip().upper() for p in data)
    if not 1 <= d.get("cols", 16) <= 40:
        raise BoardError(f"{where}: 'cols' must be 1..40")
    if d.get("rows", 2) not in (1, 2, 4):
        raise BoardError(f"{where}: 'rows' must be 1, 2 or 4")
    if d.get("rw") == "":
        del d["rw"]


def _check_oled(d: dict, where: str) -> None:
    bus = d.setdefault("bus", "twi").lower()
    if bus not in ("twi", "spi"):
        raise BoardError(f"{where}: 'bus' must be 'twi' or 'spi'")
    d["bus"] = bus
    if not 0 <= d.get("instance", 0) <= 9:
        raise BoardError(f"{where}: 'instance' must be 0..9")
    if bus == "twi":
        if d.get("addr", 0x3C) not in (0x3C, 0x3D):
            raise BoardError(f"{where}: an SSD1306 on I2C is at 0x3C (60) or 0x3D (61)")
        if d.get("cs") or d.get("dc"):
            raise BoardError(f"{where}: 'cs'/'dc' are for SPI; an I2C panel only has 'res'")
    else:
        if "addr" in d:
            raise BoardError(f"{where}: 'addr' is for I2C")
        if not d.get("dc"):
            raise BoardError(f"{where}: an SPI panel needs 'dc' (data/command pin)")
    for k in ("cs", "dc", "res"):
        if d.get(k) == "":
            del d[k]


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
    lcds: dict[int, LcdModel] = field(default_factory=dict)
    oleds: dict[int, int] = field(default_factory=dict)       # widget index -> host OLED id
    _by_pin: dict[int, list[LedModel]] = field(default_factory=dict)
    _lcd_pins: dict[int, list[tuple[LcdModel, str]]] = field(default_factory=dict)
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
                case LcdSpec(cols=cols, rows=rows):
                    self.lcds[i] = m = LcdModel(cols, rows)
                    for pin, role in w.roles().items():
                        m.init_pin(role, sim.pin_state(pin).level)
                        self._lcd_pins.setdefault(sim.pin(pin), []).append((m, role))
                        # every edge: the decoder latches on E's falling edge
                        self._subs.append(sim.subscribe_pin(pin, 0))

                case OledSpec(bus=bus, instance=inst, addr=addr, cs=cs, dc=dc, res=res):
                    self.oleds[i] = sim.attach_oled(bus, inst, addr, cs or None, dc or None,
                                                    res or None)

    def oled_state(self, index: int):
        """Current `OledState` of the OLED widget at `index`."""
        return self.sim.read_oled(self.oleds[index])

    def dispatch(self, ev) -> None:
        """Feed one event; only PinEvents for a wired LED or LCD pin matter."""
        if isinstance(ev, OverflowEvent):       # pin edges were dropped
            for i, m in self.lcds.items():
                roles = self.board.widgets[i].roles()
                m.resync({r: self.sim.pin_state(p).level for p, r in roles.items()})
            return
        pin = getattr(ev, "pin", -1)
        for m in self._by_pin.get(pin, ()):
            m.on_pin(ev.cycle, ev.level)
        for m, role in self._lcd_pins.get(pin, ()):
            m.on_pin(role, ev.level)

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
        oleds, self.oleds = self.oleds, {}
        for o in oleds.values():
            try:
                self.sim.detach_oled(o)
            except Exception:       # sim already closed
                pass
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
