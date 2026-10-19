"""Digital pin history for the GUI timeline (Qt-free)."""

import re
from bisect import bisect_right


class PinTrace:
    """Level changes of one pin, as (cycle, level) edges in cycle order."""

    def __init__(self, cycle: int, level: bool, limit: int = 200_000):
        self._cycles = [cycle]
        self._levels = [bool(level)]
        self._limit = limit

    def __len__(self) -> int:
        return len(self._cycles)

    def record(self, cycle: int, level: bool) -> None:
        level = bool(level)
        if level == self._levels[-1] or cycle < self._cycles[-1]:
            return                  # repeated level or out-of-order: nothing changed
        self._cycles.append(cycle)
        self._levels.append(level)
        if len(self._cycles) > self._limit:
            drop = self._limit // 10
            del self._cycles[:drop], self._levels[:drop]

    @property
    def first_cycle(self) -> int:
        return self._cycles[0]

    def level_at(self, cycle: int) -> bool | None:
        """Level at `cycle`; None before the first known sample."""
        i = bisect_right(self._cycles, cycle) - 1
        return self._levels[i] if i >= 0 else None

    def segments(self, t0: int, t1: int) -> list[tuple[int, int, bool]]:
        """Constant-level runs (start, end, level) clipped to [t0, t1], skipping
        any part before the first known sample. The last run ends at `t1`."""
        if t1 <= t0:
            return []
        i = max(0, bisect_right(self._cycles, t0) - 1)
        out = []
        for j in range(i, len(self._cycles)):
            start = max(self._cycles[j], t0)
            if start >= t1:
                break
            end = self._cycles[j + 1] if j + 1 < len(self._cycles) else t1
            out.append((start, min(end, t1), self._levels[j]))
        return out


_TIMER_OUT = (
    (re.compile(r"TCA(\d)\.WO([0-2])"), "tca", lambda m: int(m[2])),
    (re.compile(r"TCB(\d)\.WO"), "tcb", lambda m: 2),        # AVR_TCB_IRQ_WO
    (re.compile(r"TCD(\d)\.WO([AB])"), "tcd", lambda m: "AB".index(m[2])),
)


def timer_output(name: str) -> tuple[str, str, int] | None:
    """'TCA0.WO1' / 'TCB2.WO' / 'TCD0.WOA' -> (ioctl tag, instance char, IRQ index)
    for subscribing to the timer's waveform-output IRQ; None if not such a name."""
    for rx, tag, index in _TIMER_OUT:
        if m := rx.fullmatch(name):
            return tag, m[1], index(m)
    return None


def format_time(cycles: float, frequency: int) -> str:
    """'12.5 µs' style label for a cycle count at `frequency` Hz."""
    if frequency <= 0:
        return f"{cycles:.0f} cyc"
    s = cycles / frequency
    for unit, scale in (("s", 1.0), ("ms", 1e-3), ("µs", 1e-6)):
        if abs(s) >= scale:
            return f"{s / scale:.7g} {unit}"
    return f"{s / 1e-9:.7g} ns"


def measure(a: int | None, b: int | None, frequency: int) -> str:
    """Text for two cursor cycles: both times, their distance and 1/distance."""
    if a is None and b is None:
        return "click = cursor A, right-click = cursor B"
    parts = [f"A {format_time(a, frequency)}" if a is not None else "A -",
             f"B {format_time(b, frequency)}" if b is not None else "B -"]
    if a is not None and b is not None:
        d = abs(b - a)
        parts.append(f"Δ {format_time(d, frequency)}")
        if d and frequency > 0:
            parts.append(f"{frequency / d:.6g} Hz")
    return "   ".join(parts)


def write_vcd(path: str, traces: dict[str, "PinTrace"], frequency: int, end: int,
              uart: list[tuple[int, int]] | None = None,
              vectors: list[tuple[int, int]] | None = None) -> None:
    """Dump pin traces (and optionally UART TX bytes, interrupt vectors) as a 1 ns-resolution VCD.
    Without a known frequency the timescale is one cycle per tick (reported as 1 ns)."""
    ns = (lambda c: round(c * 1e9 / frequency)) if frequency > 0 else (lambda c: c)
    ids = {name: chr(33 + i) for i, name in enumerate(traces)}
    uart_id = chr(33 + len(traces)) if uart is not None else None
    vec_id = chr(34 + len(traces)) if vectors is not None else None
    changes: list[tuple[int, int, str]] = []        # (time, order, line)
    for name, tr in traces.items():
        for c, lv in zip(tr._cycles, tr._levels):
            changes.append((ns(c), 0, f"{int(lv)}{ids[name]}"))
    for c, byte in uart or ():
        changes.append((ns(c), 1, f"b{byte:08b} {uart_id}"))
    for c, vec in vectors or ():
        changes.append((ns(c), 1, f"b{vec:08b} {vec_id}"))
    changes.sort(key=lambda t: (t[0], t[1]))
    with open(path, "w", encoding="utf-8") as f:
        f.write("$timescale 1 ns $end\n$scope module simavr $end\n")
        for name in traces:
            f.write(f"$var wire 1 {ids[name]} {name} $end\n")
        if uart_id:
            f.write(f"$var wire 8 {uart_id} uart0_tx $end\n")
        if vec_id:
            f.write(f"$var wire 8 {vec_id} irq_vector $end\n")
        f.write("$upscope $end\n$enddefinitions $end\n")
        last = None
        for t, _, line in changes:
            if t != last:
                f.write(f"#{t}\n")
                last = t
            f.write(line + "\n")
        if last is None or ns(end) > last:
            f.write(f"#{ns(end)}\n")
