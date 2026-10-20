"""Qt-free logic of the virtual board widgets: LED, button, rotary encoder, LCD.

Each model turns pin activity into what a widget shows (LED brightness) or a
widget's user input into pin writes (button, encoder). A write is
`(pin_name, level)`; `level=None` releases the pin so the firmware's own pull-up
decides. Nothing here touches the simulator, which keeps it unit-testable.
"""

from dataclasses import dataclass, field

type Write = tuple[str, bool | None]


@dataclass(slots=True)
class LedModel:
    """Brightness of an LED from the pin's edges.

    Brightness is the fraction of simulated time the LED was lit since the
    previous `sample()`, so a PWM ramp shows as a ramp instead of flicker.
    """

    active_low: bool = False
    _lit: bool = False
    _last: int = 0
    _on: int = 0
    _total: int = 0
    _brightness: float = 0.0

    def reset(self, cycle: int, level: bool) -> None:
        self._lit = level != self.active_low
        self._last, self._on, self._total = cycle, 0, 0
        self._brightness = 1.0 if self._lit else 0.0

    def _advance(self, cycle: int) -> None:
        if cycle < self._last:          # the simulation was reset under us:
            self._last = cycle          # pins are back to inputs reading low
            self._lit = self.active_low
            self._on = self._total = 0
            return
        span = cycle - self._last
        self._total += span
        if self._lit:
            self._on += span
        self._last = cycle

    def on_pin(self, cycle: int, level: bool) -> None:
        self._advance(cycle)
        self._lit = level != self.active_low

    def sample(self, cycle: int) -> float:
        """Brightness 0..1 since the last sample. While no simulated time has
        passed (paused) the previous value stands."""
        self._advance(cycle)
        if self._total > 0:
            self._brightness = self._on / self._total
        self._on = self._total = 0
        return self._brightness


@dataclass(slots=True)
class ButtonModel:
    """Push button to ground (active_low) or to the supply.

    Released, the pin is let go so the firmware's pull-up/pull-down applies;
    with `pull` set an external resistor is modelled and the idle level is
    driven instead. `latch` makes it a toggle switch.
    """

    pin: str
    active_low: bool = True
    latch: bool = False
    pull: bool = False
    down: bool = False

    def _write(self) -> Write:
        if self.down:
            return (self.pin, not self.active_low)
        return (self.pin, self.active_low if self.pull else None)

    def idle(self) -> Write | None:
        return self._write() if self.pull else None

    def press(self) -> Write | None:
        if self.latch:
            self.down = not self.down
        elif self.down:
            return None
        else:
            self.down = True
        return self._write()

    def release(self) -> Write | None:
        if self.latch or not self.down:
            return None
        self.down = False
        return self._write()


# Quadrature phases as (A closed, B closed); one detent is one trip round them.
_GRAY = ((False, False), (True, False), (True, True), (False, True))


@dataclass(slots=True)
class EncoderModel:
    """Rotary encoder with open-collector style contacts: a closed contact pulls
    its pin to ground (active_low) or the supply, an open one is released."""

    pin_a: str
    pin_b: str
    active_low: bool = True
    phase: int = 0

    def _writes(self) -> list[Write]:
        closed = _GRAY[self.phase]
        level = not self.active_low
        return [(p, level if c else None)
                for p, c in zip((self.pin_a, self.pin_b), closed)]

    def turn(self, direction: int) -> list[list[Write]]:
        """One detent clockwise (+1) or counter-clockwise (-1): the four
        phase changes in order, each a list of pin writes to apply together.
        The caller spaces them out in time so the firmware can sample each."""
        step = 1 if direction > 0 else -1
        out = []
        for _ in range(4):
            self.phase = (self.phase + step) % 4
            out.append(self._writes())
        return out


# Glyphs of the A00 character ROM that differ from ASCII (or that we can name).
_LCD_ROM = {0x5C: "¥", 0x7E: "→", 0x7F: "←", 0xA0: " ", 0xDF: "°", 0xE4: "µ",
            0xF7: "π", 0xFF: "█"}


def lcd_glyph(code: int) -> str:
    """Text for a character code; 0..7 (and 8..15) are the custom CGRAM glyphs,
    which the caller draws from `LcdModel.cgram`, so they come back as '▯'."""
    if code in _LCD_ROM:
        return _LCD_ROM[code]
    if 0x20 <= code < 0x7E:
        return chr(code)
    return "▯"


@dataclass(slots=True)
class LcdModel:
    """HD44780 character LCD, decoded from its pins.

    Feed it every edge of the wired pins with `on_pin(role, level)`; roles are
    "rs", "e", "rw" and "d0".."d7" (a 4-bit wiring only has d4..d7). Data is
    latched on the falling edge of E, like the chip. Writes only: the busy flag
    and read-back are not modelled (firmware that polls BF reads the pins as
    inputs; use fixed delays), and a pulse with RW high is ignored. Execution
    times are not modelled either. `version` bumps whenever what the display
    shows changes.
    """

    cols: int = 16
    rows: int = 2
    ddram: bytearray = field(default_factory=lambda: bytearray(b" " * 0x80))
    cgram: bytearray = field(default_factory=lambda: bytearray(64))
    ac: int = 0                     # address counter
    in_cgram: bool = False
    shift: int = 0                  # display shift, in columns to the left
    inc: bool = True                # entry mode I/D
    entry_shift: bool = False       # entry mode S
    display_on: bool = False
    cursor_on: bool = False
    blink_on: bool = False
    eight_bit: bool = True          # interface length DL; the chip powers up 8-bit
    two_line: bool = False          # N
    version: int = 0
    lost: bool = False              # host dropped pin events: state may be wrong
    _pins: dict = field(default_factory=dict)
    _high: int | None = None        # high nibble waiting for its low half

    def init_pin(self, role: str, level: bool) -> None:
        """Record a pin's level at bind time without it counting as an edge."""
        self._pins[role] = level

    def resync(self, levels: dict[str, bool]) -> None:
        """After dropped events: adopt the pins' current levels and forget a
        half-received byte (the 4-bit pairing can't be trusted any more)."""
        self._pins.update(levels)
        self._high = None
        self.lost = True
        self.version += 1

    def on_pin(self, role: str, level: bool) -> None:
        was = self._pins.get(role, False)
        self._pins[role] = level
        if role == "e" and was and not level:
            self._strobe()

    # -- bus -----------------------------------------------------------------

    def _bits(self, lo: int, hi: int) -> int:
        return sum(1 << (i - lo) for i in range(lo, hi)
                   if self._pins.get(f"d{i}", False))

    def _strobe(self) -> None:
        if self._pins.get("rw", False):
            return
        if self.eight_bit:
            # A 4-bit wiring leaves d0..d3 unconnected: they read as 0.
            self._execute(self._bits(0, 8))
        elif self._high is None:
            self._high = self._bits(4, 8)
        else:
            value = (self._high << 4) | self._bits(4, 8)
            self._high = None
            self._execute(value)

    # -- instruction set -----------------------------------------------------

    def _step(self, forward: bool) -> None:
        if self.in_cgram:
            self.ac = (self.ac + (1 if forward else -1)) & 0x3F
        elif forward:
            self.ac = 0 if (not self.two_line and self.ac >= 0x4F) else (self.ac + 1) & 0x7F
        else:
            self.ac = (0x4F if not self.two_line else 0x67) if self.ac == 0 else self.ac - 1

    def _execute(self, v: int) -> None:
        self.version += 1
        if self._pins.get("rs", False):
            if self.in_cgram:
                self.cgram[self.ac] = v
            else:
                self.ddram[self.ac] = v
                if self.entry_shift:
                    self.shift += 1 if self.inc else -1
            self._step(self.inc)
            return
        if v & 0x80:                        # set DDRAM address
            self.in_cgram, self.ac = False, v & 0x7F
        elif v & 0x40:                      # set CGRAM address
            self.in_cgram, self.ac = True, v & 0x3F
        elif v & 0x20:                      # function set
            self.eight_bit, self.two_line = bool(v & 0x10), bool(v & 0x08)
            self._high = None
        elif v & 0x10:                      # cursor / display shift
            right = bool(v & 0x04)
            if v & 0x08:
                self.shift += -1 if right else 1
            else:
                self._step(right)
        elif v & 0x08:                      # display on/off
            self.display_on, self.cursor_on, self.blink_on = (
                bool(v & 0x04), bool(v & 0x02), bool(v & 0x01))
        elif v & 0x04:                      # entry mode
            self.inc, self.entry_shift = bool(v & 0x02), bool(v & 0x01)
        elif v & 0x02:                      # return home
            self.in_cgram, self.ac, self.shift = False, 0, 0
        elif v & 0x01:                      # clear display
            self.ddram[:] = b" " * 0x80
            self.lost = False
            self.in_cgram, self.ac, self.shift, self.inc = False, 0, 0, True

    # -- what's on the glass -------------------------------------------------

    def _addr(self, row: int, col: int) -> int:
        """DDRAM address shown at a screen position, with the shift applied."""
        base = (0, 0x40, self.cols, 0x40 + self.cols)[row] if self.rows > 1 else 0
        if not self.two_line:
            return (base + col + self.shift) % 80
        seg = base & 0x40
        return seg + (base - seg + col + self.shift) % 40

    def codes(self) -> list[list[int]]:
        """Character codes per screen row; blank while the display is off."""
        blank = 0x20
        return [[self.ddram[self._addr(r, c)] if self.display_on else blank
                 for c in range(self.cols)] for r in range(self.rows)]

    def lines(self) -> list[str]:
        return ["".join(lcd_glyph(c) for c in row) for row in self.codes()]

    def cursor_pos(self) -> tuple[int, int] | None:
        """(row, col) of the cursor, or None if it is off-screen / in CGRAM."""
        if self.in_cgram or not self.display_on:
            return None
        for r in range(self.rows):
            for c in range(self.cols):
                if self._addr(r, c) == self.ac:
                    return r, c
        return None

    def glyph_rows(self, code: int) -> list[int]:
        """The 8 rows (5 bits each) of custom character `code` (0..7, 8..15)."""
        i = (code & 7) * 8
        return [b & 0x1F for b in self.cgram[i:i + 8]]


OLED_W, OLED_H = 128, 64


def oled_pixels(state) -> bytes:
    """What the SSD1306 panel shows, as 128x64 bytes (0 dark, 1 lit), row-major from the
    top-left. `state` is `simavr_py.core.OledState` (anything with the same fields).

    Display off gives a dark panel; "entire display on" (A5h) a fully lit one. Otherwise
    RAM row `(COM + start_line) % 64` drives each COM row (COM0 is the top row, or the
    bottom one when the COM scan is remapped), segment remap flips the columns, and the
    inverse mode (A7h) swaps lit and dark. Display offset (D3h) and scrolling are not
    modelled, matching the native part."""
    if not state.on:
        return bytes(OLED_W * OLED_H)
    if state.all_on:
        return b"\x01" * (OLED_W * OLED_H)
    inv = 1 if state.inverse else 0
    vram, out = state.vram, bytearray(OLED_W * OLED_H)
    for y in range(OLED_H):
        com = OLED_H - 1 - y if state.com_flip else y
        ram = (com + state.start_line) % OLED_H
        page, bit = (ram >> 3) * OLED_W, ram & 7
        row = y * OLED_W
        for col in range(OLED_W):
            x = OLED_W - 1 - col if state.remap else col
            out[row + x] = (vram[page + col] >> bit & 1) ^ inv
    return bytes(out)
