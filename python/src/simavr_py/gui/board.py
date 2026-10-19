"""Board canvas: the virtual LEDs, buttons and encoders of a loaded board."""

import time
from collections import deque

from PySide6.QtCore import QSize, Qt, QTimer, Signal
from PySide6.QtGui import QColor, QFont, QPainter, QPen
from PySide6.QtWidgets import (
    QGridLayout, QLabel, QMessageBox, QPushButton, QVBoxLayout, QWidget,
)

from ..boards import (
    Board, BoardError, BoardRuntime, ButtonSpec, EncoderSpec, LcdSpec, LedSpec,
)
from ..core import SimavrError, Simulator
from ..widgets import LcdModel, lcd_glyph

ENCODER_STEP_MS = 3     # wall-clock spacing of the four phase changes of a detent
COLUMNS = 4


class LedWidget(QWidget):
    def __init__(self, spec: LedSpec):
        super().__init__()
        self._color = QColor(spec.color)
        if not self._color.isValid():
            self._color = QColor("red")
        self._level = 0.0
        self.setMinimumSize(QSize(34, 34))

    def set_brightness(self, level: float):
        level = round(level, 2)
        if level != self._level:
            self._level = level
            self.update()

    def paintEvent(self, _):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        off = QColor(60, 60, 60)
        lit = QColor(
            *(round(o + (c - o) * self._level)
              for o, c in zip(off.getRgb()[:3], self._color.getRgb()[:3])))
        d = min(self.width(), self.height()) - 6
        p.setPen(QPen(QColor(20, 20, 20), 2))
        p.setBrush(lit)
        p.drawEllipse((self.width() - d) // 2, (self.height() - d) // 2, d, d)


class LcdWidget(QWidget):
    """Character LCD: green glass, dark pixels. Custom glyphs (codes 0..15) are
    drawn from CGRAM as 5x8 dots; everything else as text."""

    CELL_W, CELL_H = 15, 24         # one character cell, pixels
    BLINK_S = 0.5

    def __init__(self, spec: LcdSpec):
        super().__init__()
        self._model = LcdModel(spec.cols, spec.rows)
        self._seen = (-1, False)
        self.setMinimumSize(self.sizeHint())

    def sizeHint(self) -> QSize:
        m = self._model
        return QSize(m.cols * self.CELL_W + 16, m.rows * self.CELL_H + 16)

    def set_model(self, model: LcdModel):
        self._model = model
        self._seen = (-1, False)
        self.update()

    def _phase(self) -> bool:
        return int(time.monotonic() / self.BLINK_S) % 2 == 0

    def sync(self):
        """Repaint only when the glass changed or the blink phase flipped."""
        m = self._model
        key = (m.version, m.blink_on and self._phase())
        if key != self._seen:
            self._seen = key
            self.update()

    def paintEvent(self, _):
        m, cw, ch = self._model, self.CELL_W, self.CELL_H
        p = QPainter(self)
        p.fillRect(self.rect(), QColor(40, 60, 20))
        ink = QColor(10, 20, 5) if m.display_on else QColor(55, 80, 30)
        glass = QColor(150, 180, 60) if m.display_on else QColor(100, 130, 50)
        x0, y0 = 8, 8
        font = QFont("monospace")
        font.setStyleHint(QFont.StyleHint.Monospace)
        font.setPixelSize(ch - 6)
        p.setFont(font)
        cursor = m.cursor_pos()
        flash = m.blink_on and self._phase()
        for r, row in enumerate(m.codes()):
            for c, code in enumerate(row):
                x, y = x0 + c * cw, y0 + r * ch
                p.fillRect(x, y, cw - 1, ch - 1, glass)
                inverted = flash and cursor == (r, c)
                if inverted:
                    p.fillRect(x, y, cw - 1, ch - 1, ink)
                color = glass if inverted else ink
                if code < 16:
                    dot_w, dot_h = (cw - 3) / 5, (ch - 4) / 8
                    for dy, bits in enumerate(m.glyph_rows(code)):
                        for dx in range(5):
                            if bits & (0x10 >> dx):
                                p.fillRect(round(x + 1 + dx * dot_w),
                                           round(y + 2 + dy * dot_h),
                                           max(1, round(dot_w)), max(1, round(dot_h)),
                                           color)
                else:
                    p.setPen(color)
                    p.drawText(x, y, cw - 1, ch - 1, Qt.AlignmentFlag.AlignCenter,
                               lcd_glyph(code))
                if m.cursor_on and cursor == (r, c):
                    p.fillRect(x + 1, y + ch - 4, cw - 3, 2, ink)
        if m.lost:
            p.setPen(QColor(200, 40, 40))
            p.drawText(self.rect().adjusted(0, 0, -4, -1),
                       Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignBottom,
                       "events lost")


class BoardCanvas(QWidget):
    """Lays the widgets of a Board out in a grid and wires them to a runtime."""

    failed = Signal(str)

    def __init__(self):
        super().__init__()
        self._board: Board | None = None
        self._runtime: BoardRuntime | None = None
        self._leds: dict[int, LedWidget] = {}
        self._lcds: dict[int, LcdWidget] = {}
        self._pending: deque = deque()      # encoder phase writes still to play
        self._player = QTimer(self)
        self._player.setInterval(ENCODER_STEP_MS)
        self._player.timeout.connect(self._play_step)
        self._outer = QVBoxLayout(self)
        self._title = QLabel("No board loaded (--board FILE)")
        self._outer.addWidget(self._title)
        self._host = QWidget()
        self._outer.addWidget(self._host, 1)

    @property
    def board(self) -> Board | None:
        return self._board

    def set_board(self, board: Board | None):
        self._board = board
        self._build()

    def set_sim(self, sim: Simulator | None):
        self._close_runtime()
        if sim is not None and self._board is not None:
            try:
                self._runtime = BoardRuntime(self._board, sim)
            except (BoardError, SimavrError) as e:
                self.failed.emit(str(e))
        self._build()

    def _close_runtime(self):
        self._player.stop()
        self._pending.clear()
        rt, self._runtime = self._runtime, None
        if rt is not None:
            rt.close()

    # -- layout --------------------------------------------------------------

    def _build(self):
        self._leds.clear()
        self._lcds.clear()
        old, self._host = self._host, QWidget()
        self._outer.replaceWidget(old, self._host)
        old.deleteLater()
        board = self._board
        self._title.setText(f"Board: {board.name}" if board else
                            "No board loaded (--board FILE)")
        if board is None:
            return
        grid = QGridLayout(self._host)
        enabled = self._runtime is not None
        row = col = 0
        for i, w in enumerate(board.widgets):
            wide = isinstance(w, LcdSpec)       # an LCD takes a row of its own
            if wide and col:
                row, col = row + 1, 0
            cell = QWidget()
            lay = QVBoxLayout(cell)
            lay.setContentsMargins(4, 4, 4, 4)
            match w:
                case LedSpec(label=label):
                    led = LedWidget(w)
                    self._leds[i] = led
                    lay.addWidget(led, 1)
                case ButtonSpec(label=label, latch=latch):
                    btn = QPushButton(w.pin)
                    btn.setEnabled(enabled)
                    if latch:
                        btn.setCheckable(True)      # checked state is the switch state
                        btn.toggled.connect(lambda on, i=i: self._button(i, on))
                    else:
                        btn.pressed.connect(lambda i=i: self._button(i, True))
                        btn.released.connect(lambda i=i: self._button(i, False))
                    lay.addWidget(btn)
                case LcdSpec(label=label):
                    lcd = LcdWidget(w)
                    if self._runtime is not None:
                        lcd.set_model(self._runtime.lcds[i])
                    self._lcds[i] = lcd
                    lay.addWidget(lcd, 1, Qt.AlignmentFlag.AlignCenter)
                case EncoderSpec(label=label):
                    pair = QWidget()
                    rl = QGridLayout(pair)
                    rl.setContentsMargins(0, 0, 0, 0)
                    for col, (text, d) in enumerate((("⟲", -1), ("⟳", 1))):
                        b = QPushButton(text)
                        b.setEnabled(enabled)
                        b.clicked.connect(lambda _, i=i, d=d: self._turn(i, d))
                        rl.addWidget(b, 0, col)
                    lay.addWidget(pair)
            name = QLabel(label or "")
            name.setAlignment(Qt.AlignmentFlag.AlignCenter)
            lay.addWidget(name)
            span = COLUMNS if wide else 1
            grid.addWidget(cell, row, col, 1, span)
            col += span
            if col >= COLUMNS:
                row, col = row + 1, 0
        grid.setRowStretch(grid.rowCount(), 1)

    # -- input ---------------------------------------------------------------

    def _guarded(self, fn):
        try:
            fn()
        except SimavrError as e:
            QMessageBox.warning(self, "Board", str(e))

    def _button(self, index: int, down: bool):
        rt = self._runtime
        if rt is None:
            return
        m = rt.buttons[index]
        self._guarded(lambda: rt.apply(m.press() if down else m.release()))

    def _turn(self, index: int, direction: int):
        rt = self._runtime
        if rt is None:
            return
        # Queue behind any detent still playing so phases never interleave.
        self._pending.extend(rt.encoders[index].turn(direction))
        if not self._player.isActive():
            self._player.start()

    def _play_step(self):
        rt = self._runtime
        if rt is None or not self._pending:
            self._player.stop()
            return
        writes = self._pending.popleft()
        self._guarded(lambda: [rt.apply(w) for w in writes])

    # -- driven by the main window ------------------------------------------

    def dispatch(self, ev):
        if self._runtime is not None:
            self._runtime.dispatch(ev)

    def refresh(self):
        rt = self._runtime
        if rt is None:
            return
        try:
            now = rt.sim.cycle
            for i, led in self._leds.items():
                led.set_brightness(rt.brightness(i, now))
            for lcd in self._lcds.values():
                lcd.sync()
        except SimavrError:
            pass
