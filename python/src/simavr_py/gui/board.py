"""Board canvas: the virtual LEDs, buttons and encoders of a loaded board."""

from collections import deque

from PySide6.QtCore import QSize, Qt, QTimer, Signal
from PySide6.QtGui import QColor, QPainter, QPen
from PySide6.QtWidgets import (
    QGridLayout, QLabel, QMessageBox, QPushButton, QVBoxLayout, QWidget,
)

from ..boards import (
    Board, BoardError, BoardRuntime, ButtonSpec, EncoderSpec, LedSpec,
)
from ..core import SimavrError, Simulator

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


class BoardCanvas(QWidget):
    """Lays the widgets of a Board out in a grid and wires them to a runtime."""

    failed = Signal(str)

    def __init__(self):
        super().__init__()
        self._board: Board | None = None
        self._runtime: BoardRuntime | None = None
        self._leds: dict[int, LedWidget] = {}
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
        for i, w in enumerate(board.widgets):
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
                case EncoderSpec(label=label):
                    row = QWidget()
                    rl = QGridLayout(row)
                    rl.setContentsMargins(0, 0, 0, 0)
                    for col, (text, d) in enumerate((("⟲", -1), ("⟳", 1))):
                        b = QPushButton(text)
                        b.setEnabled(enabled)
                        b.clicked.connect(lambda _, i=i, d=d: self._turn(i, d))
                        rl.addWidget(b, 0, col)
                    lay.addWidget(row)
            name = QLabel(label or "")
            name.setAlignment(Qt.AlignmentFlag.AlignCenter)
            lay.addWidget(name)
            grid.addWidget(cell, i // COLUMNS, i % COLUMNS)
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
        except SimavrError:
            pass
