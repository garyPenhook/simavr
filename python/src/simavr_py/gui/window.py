"""Main window: toolbar, board canvas, pin panel, CPU view, memory viewer, watch pane, UART console."""

import argparse
import base64
import json
import os
import re

from PySide6.QtCore import QByteArray, QSocketNotifier, Qt, QTimer, Signal
from PySide6.QtGui import QAction, QBrush, QColor, QFont, QTextCursor, QTextFormat
from PySide6.QtWidgets import (
    QApplication, QCheckBox, QComboBox, QDockWidget, QFileDialog, QFormLayout, QGridLayout,
    QHBoxLayout, QLabel, QLineEdit, QMainWindow, QMessageBox, QPlainTextEdit,
    QPushButton, QScrollArea, QSpinBox, QTableWidget, QTableWidgetItem, QTextEdit, QWidget,
)

from ..vectors import vector_label, vector_names
from ..boards import BoardError, load_board
from ..core import (
    InterruptEvent, IrqEvent, OverflowEvent, PinEvent, Simulator, SimavrError, Space, State,
    StateEvent, StopReason, UartTxEvent, WatchFlag, cores,
)
from ..disasm import Insn
from ..disasm import window as disasm_window
from ..lines import LineTable, read_elf_lines
from ..symbols import SymbolTable, read_elf_symbols
from .board import BoardCanvas
from .timeline import TimelineView

SESSION_VERSION = 5          # 5 added "timeline", 4 added "watchpoints", 3 "breakpoints", 2 "board"; older files still load
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

    def driven(self) -> dict[str, bool]:
        """Pins currently held from outside, name -> level."""
        return {n: h.isChecked() for n, (_, d, h) in self._cells.items()
                if d.isChecked()}

    def apply_driven(self, driven: dict[str, bool]):
        """Re-apply saved drives; names this MCU doesn't have are ignored."""
        for name, level in driven.items():
            cell = self._cells.get(name)
            if cell is None:
                continue
            _, drive, high = cell
            high.setChecked(bool(level))
            drive.setChecked(True)

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


def parse_watch(text: str) -> tuple[str, int, int]:
    """'[label=]addr[:width]' -> (label, data address, width in bytes).

    The address is hex (0x optional); width is 1, 2 or 4 and defaults to 1.
    """
    label, _, spec = text.rpartition("=")
    spec = spec.strip()
    addr_s, _, width_s = spec.partition(":")
    addr_s = addr_s.strip()
    try:
        addr = int(addr_s, 0 if addr_s.lower().startswith(("0x", "0b", "0o")) else 16)
        width = int(width_s) if width_s.strip() else 1
    except ValueError:
        raise ValueError(f"bad watch {text!r}: expected [label=]hexaddr[:1|2|4]") from None
    if width not in (1, 2, 4) or addr < 0:
        raise ValueError(f"bad watch {text!r}: width must be 1, 2 or 4")
    return label.strip() or f"0x{addr:04X}", addr, width


class WatchPane(QWidget):
    """Live view of chosen data-space locations (registers or SRAM), little-endian.
    A value that changed since the last repaint is highlighted."""

    CHANGED = QColor("#ffd966")

    def __init__(self):
        super().__init__()
        self._watches: list[tuple[str, int, int]] = []
        self._prev: dict[int, int] = {}
        self._edit = QLineEdit()
        self._edit.setPlaceholderText("[label=]hexaddr[:width]  e.g. porta_out=0x0405 (Enter)")
        self._edit.returnPressed.connect(self._add_from_edit)
        rm = QPushButton("Remove")
        rm.clicked.connect(self._remove_selected)
        self._table = QTableWidget(0, 4)
        self._table.setHorizontalHeaderLabels(["Name", "Addr", "Hex", "Dec"])
        self._table.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
        self._table.setSelectionBehavior(QTableWidget.SelectionBehavior.SelectRows)
        self._table.horizontalHeader().setStretchLastSection(True)
        self._table.setFont(_mono())
        top = QHBoxLayout()
        top.addWidget(self._edit)
        top.addWidget(rm)
        lay = QGridLayout(self)
        lay.addLayout(top, 0, 0)
        lay.addWidget(self._table, 1, 0)

    @property
    def watches(self) -> list[tuple[str, int, int]]:
        return list(self._watches)

    def set_watches(self, watches):
        self._watches = [tuple(w) for w in watches]
        self._prev.clear()
        self._table.setRowCount(len(self._watches))
        for row, (label, addr, _) in enumerate(self._watches):
            for col, text in enumerate((label, f"0x{addr:04X}", "-", "-")):
                self._table.setItem(row, col, QTableWidgetItem(text))

    def _add_from_edit(self):
        try:
            w = parse_watch(self._edit.text())
        except ValueError as e:
            QMessageBox.warning(self, "Watch", str(e))
            return
        self.set_watches([*self._watches, w])
        self._edit.clear()

    def _remove_selected(self):
        rows = {i.row() for i in self._table.selectedIndexes()}
        self.set_watches(w for i, w in enumerate(self._watches) if i not in rows)

    def refresh(self, sim: Simulator | None):
        if sim is None:
            return
        for row, (_, addr, width) in enumerate(self._watches):
            val = None
            try:
                if addr + width <= sim.mem_size(Space.DATA):
                    val = int.from_bytes(sim.read(Space.DATA, addr, width), "little")
            except SimavrError:
                pass
            if val is None:
                hexs = dec = "n/a"
            else:
                hexs, dec = f"0x{val:0{width * 2}X}", str(val)
            changed = val is not None and self._prev.get(addr) not in (None, val)
            self._prev[addr] = val
            for col, text in ((2, hexs), (3, dec)):
                item = self._table.item(row, col)
                if item is None:
                    continue
                if item.text() != text:
                    item.setText(text)
                item.setBackground(QBrush(self.CHANGED) if changed else QBrush())


def parse_breakpoint(text: str, symbols: SymbolTable | None = None) -> int:
    """'hexaddr' or an ELF symbol ('name' / 'name+hexoffset') -> flash byte address
    of an instruction (even). Hex is always hex ("0b10" is 0x0B10), but an exact
    symbol name wins over a bare hex reading of it, so a function called `add`
    stays reachable (write `0xadd` for the address)."""
    t = text.strip()
    if symbols is not None:
        name, sep, off = t.partition("+")
        sym = symbols.lookup(name.strip())
        if sym is not None:
            try:
                delta = int(off, 16) if sep else 0
            except ValueError:
                delta = -1
            if delta < 0 or "_" in off:
                raise ValueError(f"bad breakpoint {text!r}: expected name+hexoffset")
            return _even(sym.addr + delta, text)
    try:
        if "_" in t:
            raise ValueError(t)
        addr = int(t, 16)       # always hex: "0b10" is 0x0B10, not binary
    except ValueError:
        raise ValueError(f"bad breakpoint {text!r}: expected a hex flash address"
                         f"{' or symbol' if symbols else ''}") from None
    return _even(addr, text)


def _even(addr: int, text: str) -> int:
    if addr < 0 or addr & 1:
        raise ValueError(f"bad breakpoint {text!r}: must be an even byte address")
    return addr


class BreakpointPane(QWidget):
    """PC breakpoints by flash byte address. The pane owns the address list and
    re-arms it on every new simulator (the host ids die with the old one)."""

    HIT = QColor("#f4a6a6")

    def __init__(self):
        super().__init__()
        self._sim: Simulator | None = None
        self._addrs: list[int] = []
        self._ids: dict[int, int] = {}          # address -> host breakpoint id
        self._hit: int | None = None
        self._symbols = SymbolTable()
        self._edit = QLineEdit()
        self._edit.setPlaceholderText("hex address or symbol, e.g. 0x0120, main+0x4 (Enter)")
        self._edit.returnPressed.connect(self._add_from_edit)
        rm = QPushButton("Remove")
        rm.clicked.connect(self._remove_selected)
        self._table = QTableWidget(0, 2)
        self._table.setHorizontalHeaderLabels(["Addr", "Armed"])
        self._table.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
        self._table.setSelectionBehavior(QTableWidget.SelectionBehavior.SelectRows)
        self._table.horizontalHeader().setStretchLastSection(True)
        self._table.setFont(_mono())
        top = QHBoxLayout()
        top.addWidget(self._edit)
        top.addWidget(rm)
        lay = QGridLayout(self)
        lay.addLayout(top, 0, 0)
        lay.addWidget(self._table, 1, 0)

    @property
    def addresses(self) -> list[int]:
        return list(self._addrs)

    def set_symbols(self, symbols: SymbolTable):
        self._symbols = symbols

    def set_sim(self, sim: Simulator | None):
        """Attach (or detach) a simulator and arm every address on it."""
        self._sim = sim
        self._ids.clear()
        self._hit = None
        for addr in self._addrs:
            self._arm(addr)
        self._render()

    def set_addresses(self, addrs):
        for addr in list(self._addrs):
            self._disarm(addr)
        self._addrs = sorted(set(addrs))
        self._hit = None
        for addr in self._addrs:
            self._arm(addr)
        self._render()

    def add(self, addr: int):
        if addr not in self._addrs:
            self._addrs = sorted([*self._addrs, addr])
            self._arm(addr)
            self._render()

    def toggle(self, addr: int):
        if addr in self._addrs:
            self._disarm(addr)
            self._addrs.remove(addr)
            if self._hit == addr:
                self._hit = None
            self._render()
        else:
            self.add(addr)

    def _arm(self, addr: int):
        if self._sim is None:
            return
        try:
            self._ids[addr] = self._sim.add_breakpoint(addr)
        except SimavrError:
            pass            # out of this device's flash: stays listed, shown unarmed

    def _disarm(self, addr: int):
        bp = self._ids.pop(addr, None)
        if bp is not None and self._sim is not None:
            try:
                self._sim.remove_breakpoint(bp)
            except SimavrError:
                pass

    def mark_hit(self, bp_id: int):
        self._hit = next((a for a, i in self._ids.items() if i == bp_id), None)
        self._render()

    def clear_hit(self):
        if self._hit is not None:
            self._hit = None
            self._render()

    def _add_from_edit(self):
        try:
            addr = parse_breakpoint(self._edit.text(), self._symbols)
        except ValueError as e:
            QMessageBox.warning(self, "Breakpoint", str(e))
            return
        self.add(addr)
        self._edit.clear()

    def _remove_selected(self):
        rows = {i.row() for i in self._table.selectedIndexes()}
        gone = [a for i, a in enumerate(self._addrs) if i in rows]
        for addr in gone:
            self._disarm(addr)
        self._addrs = [a for a in self._addrs if a not in gone]
        if self._hit in gone:
            self._hit = None
        self._render()

    def _render(self):
        selected = {self._addrs[i] for i in {x.row() for x in self._table.selectedIndexes()}
                    if i < len(self._addrs)}
        self._table.setRowCount(len(self._addrs))
        for row, addr in enumerate(self._addrs):
            armed = "yes" if addr in self._ids else ("-" if self._sim is None else "no")
            for col, text in enumerate((f"0x{addr:05X}", armed)):
                item = QTableWidgetItem(text)
                if addr == self._hit:
                    item.setBackground(QBrush(self.HIT))
                self._table.setItem(row, col, item)
            if addr in selected:
                self._table.selectRow(row)


class DisasmView(QWidget):
    """Instructions around the PC, read from flash while the sim is stopped.
    Double-click a row to toggle a PC breakpoint there."""

    CURRENT = QColor("#b7d7f5")

    def __init__(self, breaks: BreakpointPane):
        super().__init__()
        self._breaks = breaks
        self._key = None            # what the table currently shows
        self._insns: list[Insn] = []
        self._symbols = SymbolTable()
        self._lines = LineTable()
        self._table = QTableWidget(0, 5)
        self._table.setHorizontalHeaderLabels(["", "Addr", "Symbol", "Source", "Instruction"])
        self._table.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
        self._table.setSelectionBehavior(QTableWidget.SelectionBehavior.SelectRows)
        self._table.verticalHeader().setVisible(False)
        self._table.horizontalHeader().setStretchLastSection(True)
        self._table.setFont(_mono())
        self._table.cellDoubleClicked.connect(self._toggle)
        lay = QGridLayout(self)
        lay.addWidget(self._table, 0, 0)

    def set_symbols(self, symbols: SymbolTable, lines: LineTable | None = None):
        self._symbols = symbols
        self._lines = lines or LineTable()
        self._key = None                # repaint on the next refresh

    def _toggle(self, row: int, _col: int):
        if row < len(self._insns):
            self._breaks.toggle(self._insns[row].addr)

    def refresh(self, sim: Simulator | None):
        if sim is None:
            if self._key is not None:
                self._key = None
                self._insns = []
                self._table.setRowCount(0)
            return
        if sim.state in (State.RUNNING, State.SLEEPING):
            self._table.setEnabled(False)       # the PC is only readable while stopped
            return
        try:
            pc = sim.cpu.pc
            size = sim.mem_size(Space.FLASH)
            key = (sim.mmcu, pc, sim.cycle, tuple(self._breaks.addresses))
            if key == self._key:
                self._table.setEnabled(True)
                return
            insns = disasm_window(lambda a, n: sim.read(Space.FLASH, a, n), pc, size)
        except SimavrError:
            return              # resumed (or reset) between the state read and the fetch
        self._key, self._insns = key, insns
        self._table.setEnabled(True)
        bps = set(self._breaks.addresses)
        self._table.setRowCount(len(insns))
        for row, ins in enumerate(insns):
            hit = self._symbols.find(ins.addr)
            label = hit[0].name if hit and hit[1] == 0 else ""
            asm = ins.text
            if ins.target is not None and (dest := self._symbols.describe(ins.target)):
                asm += f"    ; {dest}"
            src = self._lines.find(ins.addr)
            src_text = f"{os.path.basename(src.file)}:{src.line}" if src else ""
            for col, text in enumerate(("●" if ins.addr in bps else "",
                                        f"0x{ins.addr:05X}", label, src_text, asm)):
                item = QTableWidgetItem(text)
                if col == 3 and src:
                    item.setToolTip(f"{src.file}:{src.line}")
                if ins.addr == pc:
                    item.setBackground(QBrush(self.CURRENT))
                self._table.setItem(row, col, item)
            if ins.addr == pc:
                self._table.scrollToItem(self._table.item(row, 0),
                                         QTableWidget.ScrollHint.PositionAtCenter)
        self._table.resizeColumnToContents(0)
        self._table.resizeColumnToContents(1)
        self._table.resizeColumnToContents(2)
        self._table.resizeColumnToContents(3)


class _SourceText(QPlainTextEdit):
    lineDoubleClicked = Signal(int)     # 0-based block number in the shown text

    def mouseDoubleClickEvent(self, ev):
        self.lineDoubleClicked.emit(self.cursorForPosition(ev.position().toPoint()).blockNumber())
        super().mouseDoubleClickEvent(ev)


class SourceView(QWidget):
    """The source file around the line the PC is in (needs `-g` debug info).
    Double-click a line to toggle a PC breakpoint at its first instruction."""

    CURRENT = QColor("#b7d7f5")
    CONTEXT = 200           # lines shown each side of the current one

    def __init__(self, breaks: BreakpointPane):
        super().__init__()
        self._breaks = breaks
        self._lines = LineTable()
        self._files: dict[str, list[str] | None] = {}
        self._key = None
        self._shown: tuple[str, int] | None = None     # (file, first line number shown)
        self._head = QLabel("")
        self._text = _SourceText()
        self._text.lineDoubleClicked.connect(self._toggle)
        self._text.setReadOnly(True)
        self._text.setLineWrapMode(QPlainTextEdit.LineWrapMode.NoWrap)
        self._text.setFont(_mono())
        lay = QGridLayout(self)
        lay.addWidget(self._head, 0, 0)
        lay.addWidget(self._text, 1, 0)

    def set_lines(self, lines: LineTable):
        self._lines = lines
        self._files = {}                # firmware (re)loaded: sources may have changed
        self._key = None

    def _toggle(self, block: int):
        if self._shown is None:
            return
        file, first = self._shown
        addr = self._lines.address_of(file, first + block)
        if addr is not None:
            self._breaks.toggle(addr)

    def _source(self, path: str) -> list[str] | None:
        if path not in self._files:
            try:
                with open(path, encoding="utf-8", errors="replace") as f:
                    self._files[path] = f.read().splitlines()
            except OSError:
                self._files[path] = None
        return self._files[path]

    def _show(self, key, head: str, body: str, line: int | None = None):
        if key == self._key:
            return
        self._key = key
        if line is None:
            self._shown = None
        self._head.setText(head)
        self._text.setPlainText(body)
        if line is not None:
            cur = QTextCursor(self._text.document().findBlockByLineNumber(line))
            sel = QTextEdit.ExtraSelection()
            sel.cursor = cur
            sel.format.setBackground(QBrush(self.CURRENT))
            sel.format.setProperty(QTextFormat.Property.FullWidthSelection, True)
            self._text.setExtraSelections([sel])
            self._text.setTextCursor(cur)
            self._text.centerCursor()
        else:
            self._text.setExtraSelections([])

    def refresh(self, sim: Simulator | None):
        if sim is None:
            self._show(None, "", "")
            return
        if sim.state in (State.RUNNING, State.SLEEPING):
            self._text.setEnabled(False)        # the PC is only readable while stopped
            return
        try:
            pc = sim.cpu.pc
        except SimavrError:
            return              # resumed between the state read and the fetch
        self._text.setEnabled(True)
        hit = self._lines.find(pc)
        if hit is None:
            self._show(("none", pc), f"0x{pc:05X}: no source line", "")
            return
        src = self._source(hit.file)
        if src is None:
            self._show(("missing", hit.file, hit.line), f"{hit.file}:{hit.line} (file not found)", "")
            return
        first = max(0, hit.line - 1 - self.CONTEXT)
        last = min(len(src), hit.line + self.CONTEXT)
        marked = {ln.line for a in self._breaks.addresses if (ln := self._lines.find(a))
                  and ln.file == hit.file}
        body = "\n".join(f"{'●' if n + 1 in marked else ' '}{n + 1:5}  {src[n]}"
                         for n in range(first, last))
        self._show((hit.file, hit.line, first, tuple(sorted(marked))),
                   f"{hit.file}:{hit.line}", body, hit.line - 1 - first)
        self._shown = (hit.file, first + 1)


def parse_data_break(text: str) -> tuple[int, int, WatchFlag]:
    """'hexaddr[:len] [r|w|rw|c|p]' -> (addr, length, flags). Hex is always hex.
    Modes: r read, w write (default), rw both, c CPU write that changes the value,
    p any change by anyone (CPU or peripheral model)."""
    parts = text.split()
    if not 1 <= len(parts) <= 2:
        raise ValueError(f"bad data breakpoint {text!r}: expected hexaddr[:len] [r|w|rw|c|p]")
    spec, _, ln = parts[0].partition(":")
    try:
        if "_" in parts[0]:
            raise ValueError(spec)
        addr = int(spec, 16)
        length = int(ln) if ln else 1
    except ValueError:
        raise ValueError(f"bad data breakpoint {text!r}: expected hexaddr[:decimal len]") from None
    modes = {"r": WatchFlag.READ, "w": WatchFlag.WRITE,
             "rw": WatchFlag.READ | WatchFlag.WRITE, "c": WatchFlag.WRITE | WatchFlag.CHANGE, "p": WatchFlag.PERIPH}
    mode = parts[1].lower() if len(parts) == 2 else "w"
    if mode not in modes:
        raise ValueError(f"bad data breakpoint {text!r}: mode must be r, w, rw, c or p")
    if not 0 <= addr <= 0xFFFF or not 1 <= length <= 0xFFFF:
        raise ValueError(f"bad data breakpoint {text!r}: address/length out of range")
    return addr, length, modes[mode]


def _mode_text(flags: WatchFlag) -> str:
    if flags & WatchFlag.PERIPH:
        return "p"
    if flags & WatchFlag.CHANGE:
        return "c"
    return ("r" if flags & WatchFlag.READ else "") + ("w" if flags & WatchFlag.WRITE else "")


class DataBreakPane(QWidget):
    """Data-space watchpoints that pause the run. r/w/rw/c see CPU accesses only;
    p sees any change, peripheral models included. The stop lands after the
    accessing instruction (or the cycle step that changed it). Owns
    the list and re-arms it on every new simulator."""

    HIT = QColor("#f4a6a6")

    def __init__(self):
        super().__init__()
        self._sim: Simulator | None = None
        self._items: list[tuple[int, int, WatchFlag]] = []
        self._ids: dict[tuple[int, int, WatchFlag], int] = {}
        self._hit: tuple[int, int, WatchFlag] | None = None
        self._edit = QLineEdit()
        self._edit.setPlaceholderText("hexaddr[:len] [r|w|rw|c|p], e.g. 0x0400, 3f:2 rw (Enter)")
        self._edit.returnPressed.connect(self._add_from_edit)
        rm = QPushButton("Remove")
        rm.clicked.connect(self._remove_selected)
        self._table = QTableWidget(0, 4)
        self._table.setHorizontalHeaderLabels(["Addr", "Len", "Mode", "Armed"])
        self._table.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
        self._table.setSelectionBehavior(QTableWidget.SelectionBehavior.SelectRows)
        self._table.horizontalHeader().setStretchLastSection(True)
        self._table.setFont(_mono())
        top = QHBoxLayout()
        top.addWidget(self._edit)
        top.addWidget(rm)
        lay = QGridLayout(self)
        lay.addLayout(top, 0, 0)
        lay.addWidget(self._table, 1, 0)

    @property
    def items(self) -> list[tuple[int, int, WatchFlag]]:
        return list(self._items)

    def set_sim(self, sim: Simulator | None):
        self._sim = sim
        self._ids.clear()
        self._hit = None
        for it in self._items:
            self._arm(it)
        self._render()

    def set_items(self, items):
        for it in list(self._items):
            self._disarm(it)
        self._items = list(dict.fromkeys(items))
        self._hit = None
        for it in self._items:
            self._arm(it)
        self._render()

    def add(self, item: tuple[int, int, WatchFlag]):
        if item not in self._items:
            self._items.append(item)
            self._arm(item)
            self._render()

    def _arm(self, item):
        if self._sim is None:
            return
        try:
            self._ids[item] = self._sim.add_watchpoint(*item)
        except SimavrError:
            pass            # outside this device's data space, or table full: shown unarmed

    def _disarm(self, item):
        wp = self._ids.pop(item, None)
        if wp is not None and self._sim is not None:
            try:
                self._sim.remove_watchpoint(wp)
            except SimavrError:
                pass

    def mark_hit(self, wp_id: int):
        self._hit = next((it for it, i in self._ids.items() if i == wp_id), None)
        self._render()

    def clear_hit(self):
        if self._hit is not None:
            self._hit = None
            self._render()

    def _add_from_edit(self):
        try:
            item = parse_data_break(self._edit.text())
        except ValueError as e:
            QMessageBox.warning(self, "Data breakpoint", str(e))
            return
        self.add(item)
        self._edit.clear()

    def _remove_selected(self):
        rows = {i.row() for i in self._table.selectedIndexes()}
        gone = [it for i, it in enumerate(self._items) if i in rows]
        for it in gone:
            self._disarm(it)
        self._items = [it for it in self._items if it not in gone]
        if self._hit in gone:
            self._hit = None
        self._render()

    def _render(self):
        self._table.setRowCount(len(self._items))
        for row, it in enumerate(self._items):
            addr, length, flags = it
            armed = "yes" if it in self._ids else ("-" if self._sim is None else "no")
            for col, text in enumerate((f"0x{addr:04X}", str(length), _mode_text(flags), armed)):
                item = QTableWidgetItem(text)
                if it == self._hit:
                    item.setBackground(QBrush(self.HIT))
                self._table.setItem(row, col, item)


class InterruptPane(QWidget):
    """Interrupt activity: per-vector dispatch count, CPUINT level and the cycle
    of the last dispatch. Fed from the host's vector-taken events (RETIs are not
    reported), so it counts dispatches, not pending flags."""

    LEVELS = ("LVL0", "LVL1", "NMI")

    def __init__(self):
        super().__init__()
        self._stats: dict[int, list[int]] = {}      # vector -> [level, count, last cycle]
        self._names: dict[int, str] = {}
        self._dirty = False
        clear = QPushButton("Clear")
        clear.clicked.connect(self.clear)
        self._total = QLabel("0 interrupts")
        self._exec = QLabel("executing: -")
        self._table = QTableWidget(0, 4)
        self._table.setHorizontalHeaderLabels(["Vector", "Level", "Count", "Last cycle"])
        self._table.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
        self._table.horizontalHeader().setStretchLastSection(True)
        self._table.setFont(_mono())
        top = QHBoxLayout()
        top.addWidget(self._total, 1)
        top.addWidget(clear)
        lay = QGridLayout(self)
        lay.addLayout(top, 0, 0)
        lay.addWidget(self._exec, 1, 0)
        lay.addWidget(self._table, 2, 0)

    def set_names(self, names: dict[int, str]):
        self._names = names
        self._dirty = True
        self.refresh()

    def record(self, ev: InterruptEvent):
        row = self._stats.setdefault(ev.vector, [ev.level, 0, 0])
        row[0], row[1], row[2] = ev.level, row[1] + 1, ev.cycle
        self._dirty = True

    def clear(self):
        self._stats.clear()
        self._dirty = True
        self.refresh()

    @property
    def stats(self) -> dict[int, tuple[int, int, int]]:
        return {v: tuple(r) for v, r in self._stats.items()}

    CPUINT_STATUS = 0x0111          # CPUINT base 0x0110 + STATUS offset 0x01
    EXEC_BITS = ((0x80, "NMI"), (0x02, "LVL1"), (0x01, "LVL0"))

    @classmethod
    def exec_text(cls, status: int) -> str:
        """CPUINT.STATUS (NMIEX/LVL1EX/LVL0EX) as the levels being executed.
        Nested handlers set several bits; the innermost level is listed first."""
        active = [name for bit, name in cls.EXEC_BITS if status & bit]
        return "executing: " + (" > ".join(active) if active else "none")

    def refresh_exec(self, sim: Simulator | None):
        """Read CPUINT.STATUS while stopped; keep the last text while running."""
        if sim is None or sim.state in (State.RUNNING, State.SLEEPING):
            return
        try:
            self._exec.setText(self.exec_text(sim.peek(self.CPUINT_STATUS)))
        except SimavrError:
            pass                    # resumed meanwhile, or no CPUINT at that address

    def refresh(self):
        if not self._dirty:
            return
        self._dirty = False
        rows = sorted(self._stats.items())
        self._total.setText(f"{sum(r[1] for _, r in rows)} interrupts")
        self._table.setRowCount(len(rows))
        for i, (vec, (level, count, cycle)) in enumerate(rows):
            lv = self.LEVELS[level] if 0 <= level < len(self.LEVELS) else str(level)
            for col, text in enumerate((vector_label(self._names, vec), lv, str(count), str(cycle))):
                self._table.setItem(i, col, QTableWidgetItem(text))


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


_CALLS = {"call", "rcall", "icall", "eicall"}


_RETS = {"ret", "reti"}


class _NextLine:
    """A step-over-line (or step-out) in progress: single-steps until the source
    line changes (or the function returns), running over calls with a temporary
    breakpoint at the return address."""

    def __init__(self, file: str | None = None, line: int = 0, sp: int | None = None):
        self.file, self.line = file, line
        self.sp = sp                        # step-out: SP at the start; None = step-over-line
        self.was_ret = False                # step-out: the instruction just stepped was a RET
        self.steps = 0
        self.tmp: int | None = None         # id of the return-address breakpoint


class MainWindow(QMainWindow):
    def __init__(self, mmcu: str | None, freq: int, speed: float = 1.0):
        super().__init__()
        self.setWindowTitle("simavr")
        self.resize(1100, 700)
        self._sim: Simulator | None = None
        self._notifier: QSocketNotifier | None = None
        self._mmcu, self._freq, self._speed = mmcu, freq, speed
        self._line_table = LineTable()
        self._next: _NextLine | None = None

        self.pins = PinPanel()
        self.cpu = CpuView()
        self.mem = MemoryView()
        self.watch = WatchPane()
        self.breaks = BreakpointPane()
        self.databreaks = DataBreakPane()
        self.disasm = DisasmView(self.breaks)
        self.source = SourceView(self.breaks)
        self.uart = UartConsole()
        self.irqs = InterruptPane()
        self.timeline = TimelineView()
        self.board = BoardCanvas()
        self.board.failed.connect(lambda m: QMessageBox.warning(self, "Board", m))
        self._board_path: str | None = None
        self._firmware: str | None = None
        self.setCentralWidget(self.pins)
        for title, w, area in (("Board", self.board, Qt.DockWidgetArea.TopDockWidgetArea),
                               ("CPU", self.cpu, Qt.DockWidgetArea.RightDockWidgetArea),
                               ("Watch", self.watch, Qt.DockWidgetArea.RightDockWidgetArea),
                               ("Disassembly", self.disasm, Qt.DockWidgetArea.RightDockWidgetArea),
                               ("Source", self.source, Qt.DockWidgetArea.RightDockWidgetArea),
                               ("Breakpoints", self.breaks, Qt.DockWidgetArea.RightDockWidgetArea),
                               ("Data Breakpoints", self.databreaks,
                                Qt.DockWidgetArea.RightDockWidgetArea),
                               ("Interrupts", self.irqs, Qt.DockWidgetArea.RightDockWidgetArea),
                               ("Timeline", self.timeline, Qt.DockWidgetArea.BottomDockWidgetArea),
                               ("Memory", self.mem, Qt.DockWidgetArea.BottomDockWidgetArea),
                               ("UART", self.uart, Qt.DockWidgetArea.BottomDockWidgetArea)):
            dock = QDockWidget(title, self)
            dock.setObjectName(title)       # saveState() keys docks by object name
            dock.setWidget(w)
            self.addDockWidget(area, dock)

        menu = self.menuBar().addMenu("&Session")
        for label, slot in (("Open &board…", self.open_board_dialog),
                            ("&Save session…", self.save_session_dialog),
                            ("&Load session…", self.load_session_dialog)):
            act = QAction(label, self)
            act.triggered.connect(slot)
            menu.addAction(act)

        tb = self.addToolBar("Run")
        self._acts = {}
        for key, label, slot in (("open", "Open…", self.open_dialog),
                                 ("run", "Run", self.run), ("pause", "Pause", self.pause),
                                 ("step", "Step", self.step),
                                 ("next", "Next line", self.step_line),
                                 ("out", "Step out", self.step_out), ("reset", "Reset", self.reset)):
            act = QAction(label, self)
            act.triggered.connect(slot)
            tb.addAction(act)
            self._acts[key] = act
            if key == "open":
                self._add_target_controls(tb)
        self._update_actions()

        self._timer = QTimer(self)
        self._timer.timeout.connect(self.refresh)
        self._timer.start(REFRESH_MS)

    # -- target selection ----------------------------------------------------

    def _add_target_controls(self, tb):
        """MCU / clock pickers, so a bare launch can pick the device, then open an image."""
        self._mcu_box = QComboBox()
        self._mcu_box.addItem("(from ELF)", "")
        for name in sorted(cores()):
            self._mcu_box.addItem(name, name)
        self._mcu_box.setToolTip("Target MCU; changing it reloads the current firmware")
        self._freq_box = QSpinBox()
        self._freq_box.setRange(0, 100_000_000)
        self._freq_box.setSingleStep(100_000)
        self._freq_box.setSpecialValueText("auto Hz")
        self._freq_box.setSuffix(" Hz")
        self._freq_box.setToolTip("Clock frequency; 0 = take it from the ELF / device default")
        self._sync_target(self._mmcu, self._freq)
        self._mcu_box.activated.connect(self._target_changed)
        self._freq_box.editingFinished.connect(self._target_changed)
        tb.addWidget(self._mcu_box)
        tb.addWidget(self._freq_box)

    def _sync_target(self, mmcu: str | None, freq: int):
        """Show the target without triggering a reload."""
        self._mcu_box.blockSignals(True)
        self._freq_box.blockSignals(True)
        self._mcu_box.setCurrentIndex(max(0, self._mcu_box.findData(mmcu or "")))
        self._freq_box.setValue(freq or 0)
        self._mcu_box.blockSignals(False)
        self._freq_box.blockSignals(False)

    def _target_changed(self, *_):
        mmcu = self._mcu_box.currentData() or None
        freq = self._freq_box.value()
        if (mmcu, freq) == (self._mmcu, self._freq):
            return
        self._mmcu, self._freq = mmcu, freq
        if self._firmware:
            self.load(self._firmware)

    # -- session -------------------------------------------------------------

    def load(self, path: str):
        self._teardown()
        try:
            with open(path, "rb") as f:
                is_elf = f.read(4) == b"\x7fELF"
            if is_elf:
                sim = Simulator.from_elf(path, self._mmcu, self._freq)
            elif not self._mmcu:
                raise ValueError("choose an MCU in the toolbar (or --mmcu) for HEX images")
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
        try:
            sim.subscribe_interrupts()
        except SimavrError:
            pass
        self.irqs.clear()
        self._notifier = QSocketNotifier(sim.fileno(), QSocketNotifier.Type.Read, self)
        self._notifier.activated.connect(self._drain)
        self._firmware = path
        self.watch.set_watches(self.watch.watches)   # drop stale change baselines
        self.pins.set_sim(sim)
        self.uart.set_sim(sim)
        self.board.set_sim(sim)
        self.timeline.set_sim(sim)
        self.irqs.set_names(vector_names(sim.mmcu))
        self.breaks.set_sim(sim)
        self.databreaks.set_sim(sim)
        symbols = read_elf_symbols(path) if is_elf else SymbolTable()
        lines = read_elf_lines(path) if is_elf else LineTable()
        self._line_table = lines
        self.disasm.set_symbols(symbols, lines)
        self.source.set_lines(lines)
        self.breaks.set_symbols(symbols)
        self._sync_target(sim.mmcu, self._freq)
        self.setWindowTitle(f"simavr - {sim.mmcu} - {path}")
        self._update_actions()
        self.refresh()

    def _teardown(self):
        self._cancel_next()
        # Drop every reference first so no timer/notifier touches a closed handle.
        sim, self._sim = self._sim, None
        if self._notifier is not None:
            self._notifier.setEnabled(False)
            self._notifier.deleteLater()
            self._notifier = None
        self.pins.set_sim(None)
        self.uart.set_sim(None)
        self.breaks.set_sim(None)
        self.databreaks.set_sim(None)
        self.timeline.set_sim(None)
        self.board.set_sim(None)     # releases held pins while the sim is still open
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
            if isinstance(ev, StateEvent) and self._next is not None and self._next_event(ev):
                continue
            match ev:
                case UartTxEvent(index=0, byte=b):
                    tx.append(b)
                    self.timeline.record_uart(ev)
                case InterruptEvent():
                    self.irqs.record(ev)
                    self.timeline.record_interrupt(ev)
                case PinEvent() | IrqEvent() if self.timeline.owns(ev.sub):
                    self.timeline.record(ev)
                case OverflowEvent():
                    self.timeline.resync(ev.cycle)      # edges were dropped: re-read levels
                    self.board.dispatch(ev)
                case PinEvent() | OverflowEvent():
                    self.board.dispatch(ev)
                case StateEvent(reason=StopReason.BREAKPOINT, id=bp):
                    # A queued event can outlive a Run/Step pressed since; only
                    # mark while the sim is actually stopped.
                    if sim.state not in (State.RUNNING, State.SLEEPING):
                        self.breaks.mark_hit(bp)
                    self._update_actions()
                case StateEvent(reason=StopReason.WATCHPOINT, id=wp):
                    if sim.state not in (State.RUNNING, State.SLEEPING):
                        self.databreaks.mark_hit(wp)
                    self._update_actions()
                case StateEvent():
                    self._update_actions()
        if tx:
            self.uart.feed(tx)

    def refresh(self):
        sim = self._sim
        self.pins.refresh()
        self.board.refresh()
        self.cpu.refresh(sim)
        self.disasm.refresh(sim)
        self.source.refresh(sim)
        self.mem.refresh(sim)
        self.watch.refresh(sim)
        self.irqs.refresh()
        self.irqs.refresh_exec(sim)
        self.timeline.refresh()
        self._update_actions()

    # -- board ---------------------------------------------------------------

    def load_board(self, path: str, rebind: bool = True):
        """Show the board in `path`; with a running sim, rewire it to that."""
        try:
            board = load_board(path)
        except BoardError as e:
            QMessageBox.warning(self, "Board", str(e))
            return
        self._board_path = os.path.abspath(path)
        if board.mmcu and not self._mmcu:
            self._mmcu = board.mmcu
        if board.freq and not self._freq:
            self._freq = board.freq
        self.board.set_board(board)
        if rebind:
            self.board.set_sim(self._sim)

    def open_board_dialog(self):
        path, _ = QFileDialog.getOpenFileName(self, "Open board", "",
                                              "Board (*.json *.toml)")
        if path:
            self.load_board(path)

    # -- session save/restore ------------------------------------------------

    def session_dict(self) -> dict:
        b64 = lambda ba: base64.b64encode(bytes(ba)).decode()
        return {
            "version": SESSION_VERSION,
            "firmware": self._firmware,
            "board": self._board_path,
            "mmcu": self._sim.mmcu if self._sim else self._mmcu,
            "freq": self._freq,
            "speed": self._speed,
            "geometry": b64(self.saveGeometry()),
            "layout": b64(self.saveState()),
            "watches": [list(w) for w in self.watch.watches],
            "breakpoints": self.breaks.addresses,
            "watchpoints": [[a, n, int(f)] for a, n, f in self.databreaks.items],
            "driven": self.pins.driven(),
            "timeline": self.timeline.wanted,
        }

    def apply_session(self, d: dict, load_firmware: bool = True):
        """Restore a session dict. Raises ValueError on a file we can't read."""
        if not isinstance(d, dict) or d.get("version") not in (1, 2, 3, 4, SESSION_VERSION):
            raise ValueError("not a simavr session file (or unsupported version)")
        self._mmcu = d.get("mmcu") or self._mmcu
        self._freq = int(d.get("freq", self._freq))
        self._speed = float(d.get("speed", self._speed))
        self.watch.set_watches((str(l), int(a), int(w)) for l, a, w in d.get("watches", []))
        self.breaks.set_addresses(parse_breakpoint(f"{int(a):x}")
                                  for a in d.get("breakpoints", []))
        self.databreaks.set_items((int(a), int(n), WatchFlag(int(f)))
                                  for a, n, f in d.get("watchpoints", []))
        self.timeline.set_wanted(d.get("timeline", []))
        for key, restore in (("geometry", self.restoreGeometry), ("layout", self.restoreState)):
            if d.get(key):
                restore(QByteArray(base64.b64decode(d[key])))
        if d.get("board"):
            self.load_board(d["board"])
        fw = d.get("firmware")
        if load_firmware and fw:
            self.load(fw)       # builds a fresh pin panel, so drives go after
        if self._sim is not None:
            self.pins.apply_driven(d.get("driven", {}))

    def save_session_dialog(self):
        path, _ = QFileDialog.getSaveFileName(self, "Save session", "session.json",
                                              "Session (*.json)")
        if path:
            try:
                with open(path, "w") as f:
                    json.dump(self.session_dict(), f, indent=2)
            except OSError as e:
                QMessageBox.warning(self, "Save session", str(e))

    def load_session_dialog(self):
        path, _ = QFileDialog.getOpenFileName(self, "Load session", "", "Session (*.json)")
        if path:
            self.load_session(path)

    def load_session(self, path: str, load_firmware: bool = True):
        try:
            with open(path) as f:
                self.apply_session(json.load(f), load_firmware)
        except (OSError, ValueError, TypeError, KeyError) as e:
            QMessageBox.warning(self, "Load session", str(e))

    # -- actions -------------------------------------------------------------

    def _guard(self, fn):
        try:
            fn()
        except SimavrError as e:
            QMessageBox.warning(self, "simavr", str(e))
        self._update_actions()

    def open_dialog(self):
        path, _ = QFileDialog.getOpenFileName(self, "Open firmware", "",
                                              "Firmware (*.elf *.axf *.hex);;All files (*)")
        if path:
            self.load(path)

    def run(self):
        self._cancel_next()
        if self._sim:
            self._guard(self._sim.resume)
            if self._sim.state in (State.RUNNING, State.SLEEPING):
                self.breaks.clear_hit()
                self.databreaks.clear_hit()

    def pause(self):
        self._cancel_next()
        if self._sim and self._sim.state in (State.RUNNING, State.SLEEPING):
            self._guard(self._sim.pause)
        self._update_actions()

    def step(self):
        self._cancel_next()
        if self._sim:
            self._guard(self._sim.step)
            self.breaks.clear_hit()
            self.databreaks.clear_hit()

    # -- step over line ------------------------------------------------------

    def step_line(self):
        """Run to the next source line, over calls. Without line info: one step."""
        sim = self._sim
        if sim is None or self._next is not None or sim.state in (
                State.RUNNING, State.SLEEPING, State.DONE, State.CRASHED):
            return
        self._drain()       # a stop event still queued must not be mistaken for ours
        try:
            start = self._line_table.find(sim.cpu.pc)
        except SimavrError:
            return
        if start is None:
            self.step()
            return
        self._begin(_NextLine(start.file, start.line))

    def step_out(self):
        """Run until the current function returns (stops just after the RET)."""
        sim = self._sim
        if sim is None or self._next is not None or sim.state in (
                State.RUNNING, State.SLEEPING, State.DONE, State.CRASHED):
            return
        self._drain()
        try:
            sp = sim.cpu.sp
        except SimavrError:
            return
        self._begin(_NextLine(sp=sp))

    def _begin(self, nx: _NextLine):
        self.breaks.clear_hit()
        self.databreaks.clear_hit()
        self._next = nx
        self._next_advance(nx)
        self._update_actions()

    def _cancel_next(self):
        nx, self._next = self._next, None
        if nx is not None and nx.tmp is not None and self._sim is not None:
            try:
                self._sim.remove_breakpoint(nx.tmp)
            except SimavrError:
                pass
            nx.tmp = None

    def _next_event(self, ev: StateEvent) -> bool:
        """True if `ev` is one of our own stops (and the next move is scheduled)."""
        nx = self._next
        if ev.state in (State.RUNNING, State.SLEEPING):
            return True             # our own resume/step starting: wait for the stop
        ours = (ev.reason == StopReason.STEP and nx.tmp is None) or (
            ev.reason == StopReason.BREAKPOINT and nx.tmp is not None and ev.id == nx.tmp)
        if not ours:
            self._cancel_next()     # user breakpoint, watchpoint, pause, exit: stay put
            return False
        QTimer.singleShot(0, lambda: self._next_advance(nx))
        return True

    def _next_advance(self, nx: _NextLine):
        sim = self._sim
        if sim is None or self._next is not nx or sim.state in (State.RUNNING, State.SLEEPING):
            return                  # cancelled, replaced, or still moving: a stop event re-enters
        try:
            if nx.tmp is not None:
                sim.remove_breakpoint(nx.tmp)
                nx.tmp = None
            if sim.state in (State.DONE, State.CRASHED):
                self._next = None
                return
            cpu = sim.cpu
            pc = cpu.pc
            here = self._line_table.find(pc)
            if nx.sp is not None:
                # A RET that leaves our frame ends with SP above where we started
                # (an ISR's RETI restores SP to at most that, so it doesn't count).
                if nx.was_ret and cpu.sp > nx.sp:
                    self._next = None
                    return
            elif nx.steps and here is not None and (here.file, here.line) != (nx.file, nx.line):
                self._next = None
                return
            nx.steps += 1
            ins = next((i for i in disasm_window(lambda a, n: sim.read(Space.FLASH, a, n), pc,
                                                 sim.mem_size(Space.FLASH), 0, 1)
                        if i.addr == pc), None)
            if ins is not None and ins.text.split()[0] in _CALLS:
                nx.tmp = sim.add_breakpoint(pc + ins.size)
                sim.resume()
            else:
                nx.was_ret = ins is not None and ins.text.split()[0] in _RETS
                sim.step()
        except SimavrError as e:
            self._cancel_next()
            QMessageBox.warning(self, "simavr", str(e))

    def reset(self):
        self._cancel_next()
        if self._sim:
            self._guard(self._sim.reset)

    def _update_actions(self):
        sim = self._sim
        state = sim.state if sim else None
        running = state in (State.RUNNING, State.SLEEPING)
        live = sim is not None and state not in (State.DONE, State.CRASHED)
        a = self._acts
        a["run"].setEnabled(live and not running)
        a["pause"].setEnabled(live and (running or self._next is not None))
        # Stepping while the sim thread runs would race the run request.
        a["step"].setEnabled(live and not running)
        a["next"].setEnabled(live and not running and self._next is None)
        a["out"].setEnabled(live and not running and self._next is None)
        a["reset"].setEnabled(sim is not None)


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(prog="simavr_py.gui")
    ap.add_argument("firmware", nargs="?", help="ELF or HEX image")
    ap.add_argument("-m", "--mmcu", help="MCU name (needed for HEX / ELF without .mmcu)")
    ap.add_argument("-f", "--freq", type=int, default=0, help="clock frequency in Hz")
    ap.add_argument("--board", help="board definition (.json / .toml)")
    ap.add_argument("--session", help="restore a saved session (.json)")
    ap.add_argument("--speed", type=float, default=1.0,
                    help="1.0 = real time (default), 0 = unthrottled")
    args, qt_args = ap.parse_known_args(argv[1:])
    app = QApplication([argv[0], *qt_args])
    win = MainWindow(args.mmcu, args.freq, args.speed)
    win.show()
    if args.board:
        win.load_board(args.board)  # before the firmware, so its mmcu/freq apply
    if args.firmware:
        win.load(args.firmware)     # an explicit image wins over the session's
    if args.session:
        win.load_session(args.session, load_firmware=not args.firmware)
    if args.board:
        win.load_board(args.board)  # likewise explicit over the session's
    return app.exec()
