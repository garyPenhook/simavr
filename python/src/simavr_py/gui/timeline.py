"""Pin timeline dock: digital waveforms of chosen pins over simulated time."""

from PySide6.QtCore import QPointF, QRectF, Qt
from PySide6.QtGui import QColor, QFont, QPainter, QPen
from PySide6.QtWidgets import (
    QCheckBox, QComboBox, QGridLayout, QLineEdit, QPushButton, QWidget,
)

from ..core import PinEvent, SimavrError, Simulator
from ..timeline import PinTrace, format_time

SPANS = (("10 µs", 10e-6), ("100 µs", 100e-6), ("1 ms", 1e-3), ("10 ms", 10e-3),
         ("100 ms", 100e-3), ("1 s", 1.0))
LABEL_W, ROW_H, AXIS_H = 64, 28, 18


class _Canvas(QWidget):
    def __init__(self, view: "TimelineView"):
        super().__init__()
        self._v = view
        self._drag_x: float | None = None
        self.setMinimumHeight(ROW_H + AXIS_H)
        self.setMouseTracking(False)

    def paintEvent(self, _ev):
        v = self._v
        p = QPainter(self)
        pal = self.palette()
        p.fillRect(self.rect(), pal.base())
        fg, dim = pal.text().color(), pal.mid().color()
        font = QFont("monospace")
        font.setStyleHint(QFont.StyleHint.Monospace)
        p.setFont(font)
        t0, t1 = v.window()
        span = max(1, t1 - t0)
        w = max(1, self.width() - LABEL_W)
        x = lambda c: LABEL_W + (c - t0) * w / span
        for row, name in enumerate(v.names):
            top = row * ROW_H
            hi, lo = top + 5, top + ROW_H - 5
            p.setPen(fg)
            p.drawText(QRectF(2, top, LABEL_W - 6, ROW_H), Qt.AlignmentFlag.AlignVCenter, name)
            p.setPen(QPen(dim, 1, Qt.PenStyle.DotLine))
            p.drawLine(LABEL_W, int(top + ROW_H - 1), self.width(), int(top + ROW_H - 1))
            p.setPen(QPen(QColor("#2e8b57"), 1.5))
            prev = None
            for start, end, level in v.traces[name].segments(t0, t1):
                y = hi if level else lo
                if prev is not None and prev != y:
                    p.drawLine(QPointF(x(start), prev), QPointF(x(start), y))
                p.drawLine(QPointF(x(start), y), QPointF(x(end), y))
                prev = y
        axis = len(v.names) * ROW_H
        p.setPen(dim)
        p.drawLine(LABEL_W, axis + 2, self.width(), axis + 2)
        p.setPen(fg)
        f = v.frequency
        p.drawText(LABEL_W, axis + AXIS_H - 3, format_time(t0, f))
        end_text = format_time(t1, f)
        p.drawText(self.width() - 8 * (len(end_text) + 1), axis + AXIS_H - 3, end_text)
        p.end()

    def wheelEvent(self, ev):
        self._v.zoom(-1 if ev.angleDelta().y() > 0 else 1)

    def mousePressEvent(self, ev):
        self._drag_x = ev.position().x()

    def mouseMoveEvent(self, ev):
        if self._drag_x is not None:
            dx = ev.position().x() - self._drag_x
            self._drag_x = ev.position().x()
            self._v.pan(-dx / max(1, self.width() - LABEL_W))

    def mouseReleaseEvent(self, _ev):
        self._drag_x = None

    def mouseDoubleClickEvent(self, ev):
        row = int(ev.position().y() // ROW_H)
        if 0 <= row < len(self._v.names) and ev.position().x() < LABEL_W:
            self._v.remove(self._v.names[row])


class TimelineView(QWidget):
    """Pins are chosen by name; drag to pan, wheel to zoom, double-click a pin
    label to remove it. 'Follow' keeps the newest activity in view."""

    def __init__(self):
        super().__init__()
        self._sim: Simulator | None = None
        self.names: list[str] = []
        self.traces: dict[str, PinTrace] = {}
        self._subs: dict[int, str] = {}         # subscription id -> pin name
        self._wanted: list[str] = []            # survives reloads
        self._end: int | None = None            # None = follow
        self.frequency = 0
        self._edit = QLineEdit()
        self._edit.setPlaceholderText("pin to trace, e.g. PA0 (Enter)")
        self._edit.returnPressed.connect(self._add_from_edit)
        self._span = QComboBox()
        for label, _ in SPANS:
            self._span.addItem(label)
        self._span.setCurrentIndex(3)
        self._span.currentIndexChanged.connect(lambda _: self._canvas.update())
        self._follow = QCheckBox("Follow")
        self._follow.setChecked(True)
        self._follow.toggled.connect(self._follow_toggled)
        clear = QPushButton("Clear")
        clear.clicked.connect(self.clear)
        self._canvas = _Canvas(self)
        lay = QGridLayout(self)
        lay.addWidget(self._edit, 0, 0)
        lay.addWidget(self._span, 0, 1)
        lay.addWidget(self._follow, 0, 2)
        lay.addWidget(clear, 0, 3)
        lay.addWidget(self._canvas, 1, 0, 1, 4)

    # -- model ---------------------------------------------------------------

    @property
    def wanted(self) -> list[str]:
        return list(self._wanted)

    def set_wanted(self, names):
        self._unsubscribe_all()
        self._wanted = list(dict.fromkeys(str(n) for n in names))
        self._resubscribe()

    def _unsubscribe_all(self):
        for sub in self._subs:
            try:
                self._sim.unsubscribe(sub)      # else the board would see the edges twice
            except (SimavrError, AttributeError):
                pass
        self._subs = {}

    def set_sim(self, sim: Simulator | None):
        self._unsubscribe_all()
        self._sim = sim
        self.frequency = sim.frequency if sim else 0
        self._end = None
        self._follow.setChecked(True)
        self._resubscribe()

    def _resubscribe(self):
        self.names, self.traces, self._subs = [], {}, {}
        sim = self._sim
        if sim is not None:
            for name in self._wanted:
                self._start(sim, name)
        self._canvas.setMinimumHeight(max(1, len(self.names)) * ROW_H + AXIS_H)
        self._canvas.update()

    def _start(self, sim: Simulator, name: str) -> bool:
        try:
            level = sim.pin_state(name).level
            sub = sim.subscribe_pin(name, 0)
            cycle = sim.cycle
        except (SimavrError, KeyError, ValueError, TypeError):
            return False
        self.names.append(name)
        self.traces[name] = PinTrace(cycle, level)
        self._subs[sub] = name
        return True

    def add(self, name: str) -> bool:
        name = name.strip().upper()
        if not name or name in self._wanted:
            return False
        if self._sim is not None and not self._start(self._sim, name):
            return False
        self._wanted.append(name)
        self._canvas.setMinimumHeight(max(1, len(self.names)) * ROW_H + AXIS_H)
        self._canvas.update()
        return True

    def remove(self, name: str):
        if name in self._wanted:
            self._wanted.remove(name)
        for sub, n in list(self._subs.items()):
            if n == name:
                del self._subs[sub]
                try:
                    self._sim.unsubscribe(sub)
                except (SimavrError, AttributeError):
                    pass
        if name in self.names:
            self.names.remove(name)
            del self.traces[name]
        self._canvas.setMinimumHeight(max(1, len(self.names)) * ROW_H + AXIS_H)
        self._canvas.update()

    def clear(self):
        """Forget the history, keeping the pins; each restarts at its current level."""
        sim = self._sim
        if sim is None:
            return
        for name in self.names:
            try:
                self.traces[name] = PinTrace(sim.cycle, sim.pin_state(name).level)
            except SimavrError:
                pass
        self._canvas.update()

    def resync(self, cycle: int):
        """The event queue overflowed: edges were lost, so take each pin's current
        level (the true level at `cycle` is unknown; the trace is approximate here)."""
        sim = self._sim
        for name in self.names:
            try:
                self.traces[name].record(cycle, sim.pin_state(name).level)
            except (SimavrError, AttributeError):
                pass

    def owns(self, sub: int) -> bool:
        return sub in self._subs

    def record(self, ev: PinEvent):
        name = self._subs.get(ev.sub)
        if name is not None:
            self.traces[name].record(ev.cycle, ev.level)

    # -- view ----------------------------------------------------------------

    def span_cycles(self) -> int:
        return max(1, round(SPANS[self._span.currentIndex()][1] * (self.frequency or 1_000_000)))

    def window(self) -> tuple[int, int]:
        end = self._end
        if end is None:
            try:
                end = self._sim.cycle if self._sim else 0
            except SimavrError:
                end = max((t._cycles[-1] for t in self.traces.values()), default=0)
        return max(0, end - self.span_cycles()), max(end, 1)

    def zoom(self, steps: int):
        i = min(len(SPANS) - 1, max(0, self._span.currentIndex() + steps))
        self._span.setCurrentIndex(i)

    def pan(self, fraction: float):
        """Shift the window by a fraction of its width (leaves follow mode)."""
        t0, t1 = self.window()
        self._end = max(self.span_cycles(), t1 + round(fraction * (t1 - t0)))
        self._follow.blockSignals(True)
        self._follow.setChecked(False)
        self._follow.blockSignals(False)
        self._canvas.update()

    def _follow_toggled(self, on: bool):
        self._end = None if on else self.window()[1]
        self._canvas.update()

    def _add_from_edit(self):
        if self.add(self._edit.text()):
            self._edit.clear()

    def refresh(self):
        if self.names:
            self._canvas.update()
