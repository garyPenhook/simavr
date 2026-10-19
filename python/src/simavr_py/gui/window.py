"""Main window: toolbar, pin panel, CPU view, memory viewer, UART console."""

import argparse
import re

from PySide6.QtCore import QSocketNotifier, Qt, QTimer
from PySide6.QtGui import QAction, QFont, QTextCursor
from PySide6.QtWidgets import (
    QApplication, QCheckBox, QDockWidget, QFileDialog, QFormLayout, QGridLayout,
    QHBoxLayout, QLabel, QLineEdit, QMainWindow, QMessageBox, QPlainTextEdit,
    QPushButton, QScrollArea, QSpinBox, QWidget,
)

from ..core import (
    Simulator, SimavrError, Space, State, StateEvent, UartTxEvent,
)

REFRESH_MS = 50          # repaint cadence, independent of simulated time
DRAIN_LIMIT = 1024


def _mono() -> QFont:
    f = QFont("monospace")      # needs a QGuiApplication, so never at import time
    f.setStyleHint(QFont.StyleHint.Monospace)
    return f


def _pin_sort_key(name: str):
    m = re.fullmatch(r"P([A-Z])(\d+)", name)
    return (m[1], int(m[2])) if m else (name, 0)


class PinPanel(QScrollArea):
    """One cell per pin: coloured level indicator, direction, and a toggle that
    drives the pin from outside while it is an input."""

    def __init__(self):
        super().__init__()
        self.setWidgetResizable(True)
        self._cells: dict[str, tuple[QLabel, QCheckBox, QCheckBox]] = {}
        self._sim: Simulator | None = None

    def set_sim(self, sim: Simulator | None):
        self._sim = sim
        host = QWidget()
        grid = QGridLayout(host)
        self._cells.clear()
        if sim is not None:
            names = sorted(sim.pins, key=_pin_sort_key)
            for i, name in enumerate(names):
                led = QLabel(f"{name}\n-")
                led.setAlignment(Qt.AlignmentFlag.AlignCenter)
                led.setMinimumWidth(46)
                led.setFont(_mono())
                drive = QCheckBox("drive")
                high = QCheckBox("H")
                drive.toggled.connect(lambda _, n=name: self._drive(n))
                high.toggled.connect(lambda _, n=name: self._drive(n))
                cell = QWidget()
                lay = QHBoxLayout(cell)
                lay.setContentsMargins(2, 2, 2, 2)
                lay.addWidget(led)
                lay.addWidget(drive)
                lay.addWidget(high)
                grid.addWidget(cell, i // 4, i % 4)
                self._cells[name] = (led, drive, high)
        grid.setRowStretch(grid.rowCount(), 1)
        self.setWidget(host)

    def _drive(self, name: str):
        """'drive' ticked: hold the pin at the 'H' level; unticked: release."""
        if self._sim is None:
            return
        _, drive, high = self._cells[name]
        try:
            if drive.isChecked():
                self._sim.drive(name, high.isChecked())
            else:
                self._sim.release(name)
        except SimavrError as e:
            QMessageBox.warning(self, "Pin", str(e))

    def refresh(self):
        if self._sim is None:
            return
        for name, (led, _, _) in self._cells.items():
            try:
                st = self._sim.pin_state(name)
            except SimavrError:
                return
            kind = "OUT" if st.dir else ("Z" if st.floating else "IN")
            color = "#2ecc40" if st.level else "#555555"
            text = f"{name}\n{kind} {int(st.level)}"
            if led.text() == text:
                continue
            led.setText(text)
            led.setStyleSheet(f"background:{color};color:white;border-radius:4px;")


class CpuView(QWidget):
    def __init__(self):
        super().__init__()
        form = QFormLayout(self)
        self._labels = {k: QLabel("-") for k in ("State", "Cycle", "Time", "PC",
                                                 "SP", "SREG")}
        for k, lab in self._labels.items():
            lab.setFont(_mono())
            form.addRow(k, lab)
        self._regs = QPlainTextEdit()
        self._regs.setReadOnly(True)
        self._regs.setFont(_mono())
        form.addRow(self._regs)

    def refresh(self, sim: Simulator | None):
        if sim is None:
            return
        L = self._labels
        state = sim.state
        L["State"].setText(state.name)
        L["Cycle"].setText(str(sim.cycle))
        L["Time"].setText(f"{sim.time_ns / 1e6:.3f} ms")
        if state in (State.RUNNING, State.SLEEPING):
            # The C API only exposes PC/SP/SREG/r[] while paused; keep the last
            # snapshot on screen rather than racing the sim thread.
            self._regs.setEnabled(False)
            for k in ("PC", "SP", "SREG"):
                if not L[k].text().endswith("(stale)"):
                    L[k].setText(L[k].text() + " (stale)")
            return
        try:
            cpu = sim.cpu
        except SimavrError:
            return              # resumed between the state read and the fetch
        self._regs.setEnabled(True)
        flags = "".join(c if cpu.sreg & (0x80 >> i) else "-"
                        for i, c in enumerate("ITHSVNZC"))
        L["PC"].setText(f"0x{cpu.pc:05X}")
        L["SP"].setText(f"0x{cpu.sp:04X}")
        L["SREG"].setText(f"0x{cpu.sreg:02X}  {flags}")
        rows = ("  ".join(f"r{r + c:<2}={cpu.r[r + c]:02X}" for c in range(4))
                for r in range(0, 32, 4))
        self._regs.setPlainText("\n".join(rows))


class MemoryView(QWidget):
    SPACES = (Space.DATA, Space.FLASH, Space.EEPROM)

    def __init__(self):
        super().__init__()
        bar_lay = QHBoxLayout()
        lay = bar_lay
        self._addr = QLineEdit("0x0000")
        self._addr.setMaximumWidth(90)
        self._len = QSpinBox()
        self._len.setRange(16, 4096)
        self._len.setValue(256)
        self._len.setSingleStep(16)
        self._space = 0
        self._last: str | None = None
        self._buttons = []
        for i, sp in enumerate(self.SPACES):
            b = QPushButton(sp.name.lower())
            b.setCheckable(True)
            b.setChecked(i == 0)
            b.clicked.connect(lambda _, i=i: self._pick(i))
            self._buttons.append(b)
            lay.addWidget(b)
        lay.addWidget(QLabel("addr"))
        lay.addWidget(self._addr)
        lay.addWidget(QLabel("len"))
        lay.addWidget(self._len)
        self._text = QPlainTextEdit()
        self._text.setReadOnly(True)
        self._text.setFont(_mono())
        self._text.setLineWrapMode(QPlainTextEdit.LineWrapMode.NoWrap)
        box = QGridLayout(self)
        box.addLayout(bar_lay, 0, 0)
        box.addWidget(self._text, 1, 0)

    def _pick(self, i: int):
        self._space = i
        self._last = None
        for j, b in enumerate(self._buttons):
            b.setChecked(j == i)

    def refresh(self, sim: Simulator | None):
        if sim is None:
            return
        try:
            txt = self._addr.text().strip()
            start = int(txt, 16 if not txt.lower().startswith(("0x", "0b", "0o")) else 0)
        except ValueError:
            self._text.setPlainText("bad address (hex)")
            self._last = None
            return
        space = self.SPACES[self._space]
        try:
            size = sim.mem_size(space)
            n = min(self._len.value(), max(0, size - start))
            data = sim.read(space, start, n) if n > 0 else b""
        except SimavrError as e:
            self._text.setPlainText(str(e))
            return
        lines = []
        for off in range(0, len(data), 16):
            chunk = data[off:off + 16]
            hexes = " ".join(f"{b:02X}" for b in chunk)
            text = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
            lines.append(f"{start + off:05X}  {hexes:<47}  {text}")
        out = "\n".join(lines)
        if out == self._last:       # keep selection/scroll when nothing changed
            return
        self._last = out
        bar = self._text.verticalScrollBar()
        pos = bar.value()
        self._text.setPlainText(out)
        bar.setValue(pos)


class UartConsole(QWidget):
    def __init__(self):
        super().__init__()
        self._sim: Simulator | None = None
        lay = QGridLayout(self)
        self._out = QPlainTextEdit()
        self._out.setReadOnly(True)
        self._out.setFont(_mono())
        self._out.setMaximumBlockCount(5000)
        self._in = QLineEdit()
        self._in.setPlaceholderText("send to USART0 (Enter)")
        self._in.returnPressed.connect(self._send)
        lay.addWidget(self._out, 0, 0)
        lay.addWidget(self._in, 1, 0)

    def set_sim(self, sim: Simulator | None):
        self._sim = sim
        self._out.clear()

    def feed(self, data: bytes):
        """Append a batch of bytes with one widget update."""
        cur = self._out.textCursor()
        cur.movePosition(QTextCursor.MoveOperation.End)
        cur.insertText("".join(chr(b) if b in (9, 10) or 32 <= b < 127
                               else f"\\x{b:02x}" for b in data))
        self._out.setTextCursor(cur)
        self._out.ensureCursorVisible()

    def _send(self):
        if self._sim is None:
            return
        try:
            self._sim.uart_send(0, self._in.text() + "\n")
        except SimavrError as e:
            QMessageBox.warning(self, "UART", str(e))
        self._in.clear()


class MainWindow(QMainWindow):
    def __init__(self, mmcu: str | None, freq: int, speed: float = 1.0):
        super().__init__()
        self.setWindowTitle("simavr")
        self.resize(1100, 700)
        self._sim: Simulator | None = None
        self._notifier: QSocketNotifier | None = None
        self._mmcu, self._freq, self._speed = mmcu, freq, speed

        self.pins = PinPanel()
        self.cpu = CpuView()
        self.mem = MemoryView()
        self.uart = UartConsole()
        self.setCentralWidget(self.pins)
        for title, w, area in (("CPU", self.cpu, Qt.DockWidgetArea.RightDockWidgetArea),
                               ("Memory", self.mem, Qt.DockWidgetArea.BottomDockWidgetArea),
                               ("UART", self.uart, Qt.DockWidgetArea.BottomDockWidgetArea)):
            dock = QDockWidget(title, self)
            dock.setWidget(w)
            self.addDockWidget(area, dock)

        tb = self.addToolBar("Run")
        self._acts = {}
        for key, label, slot in (("open", "Open…", self.open_dialog),
                                 ("run", "Run", self.run), ("pause", "Pause", self.pause),
                                 ("step", "Step", self.step), ("reset", "Reset", self.reset)):
            act = QAction(label, self)
            act.triggered.connect(slot)
            tb.addAction(act)
            self._acts[key] = act
        self._update_actions()

        self._timer = QTimer(self)
        self._timer.timeout.connect(self.refresh)
        self._timer.start(REFRESH_MS)

    # -- session -------------------------------------------------------------

    def load(self, path: str):
        self._teardown()
        try:
            with open(path, "rb") as f:
                is_elf = f.read(4) == b"\x7fELF"
            if is_elf:
                sim = Simulator.from_elf(path, self._mmcu, self._freq)
            elif not self._mmcu:
                raise ValueError("--mmcu is required for HEX images")
            else:
                sim = Simulator(self._mmcu, self._freq)
        except (SimavrError, ValueError, OSError) as e:
            QMessageBox.critical(self, "Load failed", str(e))
            return
        try:
            if not is_elf:
                sim.load_hex(path)
            sim.set_speed(self._speed)
            sim.start()
        except (SimavrError, ValueError, OSError) as e:
            sim.close()
            QMessageBox.critical(self, "Load failed", str(e))
            return
        self._sim = sim
        try:
            sim.subscribe_uart(0)       # the console shows and feeds USART0 only
        except SimavrError:
            pass
        self._notifier = QSocketNotifier(sim.fileno(), QSocketNotifier.Type.Read, self)
        self._notifier.activated.connect(self._drain)
        self.pins.set_sim(sim)
        self.uart.set_sim(sim)
        self.setWindowTitle(f"simavr - {sim.mmcu} - {path}")
        self._update_actions()
        self.refresh()

    def _teardown(self):
        # Drop every reference first so no timer/notifier touches a closed handle.
        sim, self._sim = self._sim, None
        if self._notifier is not None:
            self._notifier.setEnabled(False)
            self._notifier.deleteLater()
            self._notifier = None
        self.pins.set_sim(None)
        self.uart.set_sim(None)
        if sim is not None:
            try:
                sim.pause()
                sim.wait_stopped(1.0)
            except SimavrError:
                pass
            sim.close()
        self._update_actions()

    def closeEvent(self, ev):
        self._timer.stop()
        self._teardown()
        super().closeEvent(ev)

    # -- events --------------------------------------------------------------

    def _drain(self):
        sim = self._sim
        if sim is None:
            return
        # Bounded per wake-up so a chatty firmware can't starve the event loop;
        # the fd stays readable while events remain, so the rest comes next time.
        tx = bytearray()
        for ev in sim.events(limit=DRAIN_LIMIT):
            match ev:
                case UartTxEvent(index=0, byte=b):
                    tx.append(b)
                case StateEvent():
                    self._update_actions()
        if tx:
            self.uart.feed(tx)

    def refresh(self):
        sim = self._sim
        self.pins.refresh()
        self.cpu.refresh(sim)
        self.mem.refresh(sim)
        self._update_actions()

    # -- actions -------------------------------------------------------------

    def _guard(self, fn):
        try:
            fn()
        except SimavrError as e:
            QMessageBox.warning(self, "simavr", str(e))
        self._update_actions()

    def open_dialog(self):
        path, _ = QFileDialog.getOpenFileName(self, "Open firmware", "",
                                              "Firmware (*.elf *.hex);;All files (*)")
        if path:
            self.load(path)

    def run(self):
        if self._sim:
            self._guard(self._sim.resume)

    def pause(self):
        if self._sim:
            self._guard(self._sim.pause)

    def step(self):
        if self._sim:
            self._guard(self._sim.step)

    def reset(self):
        if self._sim:
            self._guard(self._sim.reset)

    def _update_actions(self):
        sim = self._sim
        state = sim.state if sim else None
        running = state in (State.RUNNING, State.SLEEPING)
        live = sim is not None and state not in (State.DONE, State.CRASHED)
        a = self._acts
        a["run"].setEnabled(live and not running)
        a["pause"].setEnabled(live and running)
        # Stepping while the sim thread runs would race the run request.
        a["step"].setEnabled(live and not running)
        a["reset"].setEnabled(sim is not None)


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(prog="simavr_py.gui")
    ap.add_argument("firmware", nargs="?", help="ELF or HEX image")
    ap.add_argument("-m", "--mmcu", help="MCU name (needed for HEX / ELF without .mmcu)")
    ap.add_argument("-f", "--freq", type=int, default=0, help="clock frequency in Hz")
    ap.add_argument("--speed", type=float, default=1.0,
                    help="1.0 = real time (default), 0 = unthrottled")
    args, qt_args = ap.parse_known_args(argv[1:])
    app = QApplication([argv[0], *qt_args])
    win = MainWindow(args.mmcu, args.freq, args.speed)
    win.show()
    if args.firmware:
        win.load(args.firmware)
    return app.exec()
