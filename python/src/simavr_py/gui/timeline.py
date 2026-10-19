"""Pin timeline dock: digital waveforms of chosen pins over simulated time."""

from PySide6.QtCore import QPointF, QRectF, Qt
from PySide6.QtGui import QColor, QFont, QPainter, QPen
from PySide6.QtWidgets import (
    QCheckBox, QComboBox, QFileDialog, QGridLayout, QLabel, QLineEdit, QMessageBox,
    QPushButton, QWidget,
)

from ..core import (
    InterruptEvent, IrqEvent, PinEvent, SimavrError, Simulator, UartTxEvent, ioctl_def,
)
from ..timeline import PinTrace, format_time, measure, timer_output, write_vcd

SPANS = (("10 µs", 10e-6), ("100 µs", 100e-6), ("1 ms", 1e-3), ("10 ms", 10e-3),
         ("100 ms", 100e-3), ("1 s", 1.0))
LABEL_W, ROW_H, AXIS_H = 64, 28, 18


class _Canvas(QWidget):
    def __init__(self, view: "TimelineView"):
        super().__init__()
        self._v = view
        self._drag_x: float | None = None
        self._press_x = 0.0
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
        rows = len(v.names)
        if v.show_uart:
            top = rows * ROW_H
            p.setPen(fg)
            p.drawText(QRectF(2, top, LABEL_W - 6, ROW_H), Qt.AlignmentFlag.AlignVCenter, "UART0 TX")
            p.setPen(QPen(dim, 1, Qt.PenStyle.DotLine))
            p.drawLine(LABEL_W, int(top + ROW_H - 1), self.width(), int(top + ROW_H - 1))
            p.setPen(QPen(QColor("#b5651d"), 1))
            last_x = -1e9
            for c, byte in v.uart_bytes(t0, t1):
                bx = x(c)
                if bx - last_x < 14:                # too dense to label: mark only
                    p.drawLine(QPointF(bx, top + 6), QPointF(bx, top + ROW_H - 6))
                    continue
                last_x = bx
                text = chr(byte) if 32 <= byte < 127 else f"{byte:02X}"
                p.drawRect(QRectF(bx, top + 4, 14, ROW_H - 10))
                p.drawText(QRectF(bx, top + 4, 14, ROW_H - 10), Qt.AlignmentFlag.AlignCenter, text)
            rows += 1
        if v.show_irq:
            top = rows * ROW_H
            p.setPen(fg)
            p.drawText(QRectF(2, top, LABEL_W - 6, ROW_H), Qt.AlignmentFlag.AlignVCenter, "IRQ")
            p.setPen(QPen(dim, 1, Qt.PenStyle.DotLine))
            p.drawLine(LABEL_W, int(top + ROW_H - 1), self.width(), int(top + ROW_H - 1))
            p.setPen(QPen(QColor("#7b4fa8"), 1))
            last_x = -1e9
            for c, vec in v.vectors(t0, t1):
                bx = x(c)
                p.drawLine(QPointF(bx, top + 6), QPointF(bx, top + ROW_H - 6))
                if bx - last_x >= 20:
                    last_x = bx
                    p.drawText(QPointF(bx + 2, top + ROW_H - 8), str(vec))
            rows += 1
        for c, color in zip(v.cursors, ("#d62728", "#1f77b4")):
            if c is not None and t0 <= c <= t1:
                p.setPen(QPen(QColor(color), 1))
                p.drawLine(QPointF(x(c), 0), QPointF(x(c), rows * ROW_H))
        axis = rows * ROW_H
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
        self._drag_x = self._press_x = ev.position().x()

    def mouseMoveEvent(self, ev):
        if self._drag_x is not None:
            dx = ev.position().x() - self._drag_x
            self._drag_x = ev.position().x()
            self._v.pan(-dx / max(1, self.width() - LABEL_W))

    def mouseReleaseEvent(self, ev):
        self._drag_x = None
        if abs(ev.position().x() - self._press_x) < 3:          # a click, not a pan
            which = 1 if ev.button() == Qt.MouseButton.RightButton else 0
            self._v.place_cursor(which, ev.position().x(), self.width())

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
        self.cursors: list[int | None] = [None, None]
        self.show_uart = True
        self.show_irq = True
        self._uart: list[tuple[int, int]] = []  # (cycle, byte) of USART0 TX
        self._vectors: list[tuple[int, int]] = []   # (cycle, vector number) of interrupts taken
        self._edit = QLineEdit()
        self._edit.setPlaceholderText("pin or timer output: PA0, TCA0.WO1, TCB0.WO, TCD0.WOA")
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
        export = QPushButton("Export VCD…")
        export.clicked.connect(self.export_dialog)
        self._info = QLabel(measure(None, None, 0))
        self._canvas = _Canvas(self)
        lay = QGridLayout(self)
        lay.addWidget(self._edit, 0, 0)
        lay.addWidget(self._span, 0, 1)
        lay.addWidget(self._follow, 0, 2)
        lay.addWidget(clear, 0, 3)
        lay.addWidget(export, 0, 4)
        lay.addWidget(self._canvas, 1, 0, 1, 5)
        lay.addWidget(self._info, 2, 0, 1, 5)
        self._resize_canvas()

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
        self._uart = []
        self._vectors = []
        self.cursors = [None, None]
        self._update_info()
        self._follow.setChecked(True)
        self._resubscribe()

    def _resubscribe(self):
        self.names, self.traces, self._subs = [], {}, {}
        sim = self._sim
        if sim is not None:
            for name in self._wanted:
                self._start(sim, name)
        self._resize_canvas()
        self._canvas.update()

    def _resize_canvas(self):
        rows = len(self.names) + (1 if self.show_uart else 0) + (1 if self.show_irq else 0)
        self._canvas.setMinimumHeight(max(1, rows) * ROW_H + AXIS_H)

    def _start(self, sim: Simulator, name: str) -> bool:
        try:
            if (t := timer_output(name)) is not None:
                level = False           # no way to read a timer output: assume low until it moves
                sub = sim.subscribe_irq(ioctl_def(t[0], t[1]), t[2])
            else:
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
        self._resize_canvas()
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
        self._resize_canvas()
        self._canvas.update()

    def clear(self):
        """Forget the history, keeping the pins; each restarts at its current level."""
        sim = self._sim
        self._uart = []
        self._vectors = []
        if sim is None:
            return
        for name in self.names:
            try:
                level = False if timer_output(name) else sim.pin_state(name).level
                self.traces[name] = PinTrace(sim.cycle, level)
            except SimavrError:
                pass
        self._canvas.update()

    def resync(self, cycle: int):
        """The event queue overflowed: edges were lost, so take each pin's current
        level (the true level at `cycle` is unknown; the trace is approximate here)."""
        sim = self._sim
        for name in self.names:
            if timer_output(name):
                continue                # IRQ values aren't dropped by the pin queue limits
            try:
                self.traces[name].record(cycle, sim.pin_state(name).level)
            except (SimavrError, AttributeError):
                pass

    def record_uart(self, ev: UartTxEvent):
        """A byte USART0 finished sending (shown at the cycle it left the shifter)."""
        self._uart.append((ev.cycle, ev.byte))
        if len(self._uart) > 20_000:
            del self._uart[:2_000]

    def uart_bytes(self, t0: int, t1: int) -> list[tuple[int, int]]:
        return [(c, b) for c, b in self._uart if t0 <= c <= t1]

    def owns(self, sub: int) -> bool:
        return sub in self._subs

    def record(self, ev: PinEvent | IrqEvent):
        name = self._subs.get(ev.sub)
        if name is not None:
            level = ev.level if isinstance(ev, PinEvent) else bool(ev.value)
            self.traces[name].record(ev.cycle, level)

    def record_interrupt(self, ev: InterruptEvent):
        self._vectors.append((ev.cycle, ev.vector))
        if len(self._vectors) > 20_000:
            del self._vectors[:2_000]

    def vectors(self, t0: int, t1: int) -> list[tuple[int, int]]:
        return [(c, v) for c, v in self._vectors if t0 <= c <= t1]

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

    def place_cursor(self, which: int, x: float, width: int):
        t0, t1 = self.window()
        frac = min(1.0, max(0.0, (x - LABEL_W) / max(1, width - LABEL_W)))
        self.cursors[which] = round(t0 + frac * (t1 - t0))
        self._update_info()
        self._canvas.update()

    def _update_info(self):
        self._info.setText(measure(*self.cursors, self.frequency))

    def export_dialog(self):
        path, _ = QFileDialog.getSaveFileName(self, "Export VCD", "timeline.vcd", "VCD (*.vcd)")
        if path:
            try:
                self.export_vcd(path)
            except OSError as e:
                QMessageBox.warning(self, "Export VCD", str(e))

    def export_vcd(self, path: str):
        end = self.window()[1]
        write_vcd(path, self.traces, self.frequency, end,
                  self._uart if self.show_uart else None,
                  self._vectors if self.show_irq else None)

    def _follow_toggled(self, on: bool):
        self._end = None if on else self.window()[1]
        self._canvas.update()

    def _add_from_edit(self):
        if self.add(self._edit.text()):
            self._edit.clear()

    def refresh(self):
        if self.names:
            self._canvas.update()
