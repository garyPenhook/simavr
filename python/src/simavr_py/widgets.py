"""Qt-free logic of the virtual board widgets: LED, button, rotary encoder.

Each model turns pin activity into what a widget shows (LED brightness) or a
widget's user input into pin writes (button, encoder). A write is
`(pin_name, level)`; `level=None` releases the pin so the firmware's own pull-up
decides. Nothing here touches the simulator, which keeps it unit-testable.
"""

from dataclasses import dataclass

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
