"""Digital pin history for the GUI timeline (Qt-free)."""

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


def format_time(cycles: float, frequency: int) -> str:
    """'12.5 µs' style label for a cycle count at `frequency` Hz."""
    if frequency <= 0:
        return f"{cycles:.0f} cyc"
    s = cycles / frequency
    for unit, scale in (("s", 1.0), ("ms", 1e-3), ("µs", 1e-6)):
        if abs(s) >= scale:
            return f"{s / scale:.7g} {unit}"
    return f"{s / 1e-9:.7g} ns"
