"""Help window: a topic list beside an HTML page. Opened from the Help menu (F1)."""

from PySide6.QtCore import Qt
from PySide6.QtWidgets import (
    QDialog, QHBoxLayout, QLineEdit, QListWidget, QTextBrowser, QVBoxLayout,
)

# (title, html). Keep in step with python/README.md "Using it".
TOPICS: list[tuple[str, str]] = [
    ("Getting started", """
<h2>Getting started</h2>
<p>The simulator runs AVR firmware (an ELF or HEX image) on a simulated modern AVR
(tinyAVR 0/1/2-series, megaAVR 0-series) or a classic AVR such as the ATtiny13A. Everything you see is a live view of that
simulation.</p>
<ol>
<li>Pick the <b>MCU</b> in the toolbar box, or leave <code>(from ELF)</code> to use the
device named inside the ELF. A HEX image, or an ELF without an <code>.mmcu</code>
section, needs an explicit MCU.</li>
<li>Press <b>Open&hellip;</b> and choose the firmware. Build with <code>-g</code> to get
source lines and symbols.</li>
<li>Optionally load a <b>Board</b> (virtual LEDs, buttons, LCD, OLED) from the toolbar or
<i>Session &rarr; Open board&hellip;</i>.</li>
<li>Press <b>Run</b>. Watch pins in the central panel, the Timeline, the UART console and
the register docks.</li>
</ol>
<h3>Command line</h3>
<pre>python -m simavr_py.gui [firmware] [-m MCU] [-f HZ] [--board FILE] [--session FILE] [--speed X]</pre>
<ul>
<li><code>-m</code> MCU name; <code>-f</code> clock in Hz (0 = default).</li>
<li><code>--board</code> board file (.json/.toml), loaded before the firmware.</li>
<li><code>--session</code> restore a saved session; an explicit firmware argument wins over
the session's.</li>
<li><code>--speed</code> 1.0 = real time (default), 0 = unthrottled.</li>
</ul>
"""),
    ("Toolbar & run control", """
<h2>Toolbar &amp; run control</h2>
<table border="1" cellpadding="4" cellspacing="0">
<tr><th>Control</th><th>What it does</th></tr>
<tr><td><b>Open&hellip;</b></td><td>Load firmware (ELF or multipart HEX).</td></tr>
<tr><td>MCU box</td><td>Target device. Changing it reloads the loaded firmware.</td></tr>
<tr><td>Clock box</td><td>CPU clock in Hz. <i>auto</i> (0) takes it from the ELF or the device default.</td></tr>
<tr><td><b>Board&hellip;</b></td><td>Load a board definition.</td></tr>
<tr><td><b>Run</b></td><td>Run until paused or a breakpoint/watchpoint stops it.</td></tr>
<tr><td><b>Run for</b> + ms box</td><td>Run that much <i>simulated</i> time, then pause.</td></tr>
<tr><td><b>Pause</b></td><td>Stop the run.</td></tr>
<tr><td><b>Step</b></td><td>Execute one instruction.</td></tr>
<tr><td><b>Next line</b></td><td>Step over a source line or call (needs <code>-g</code>).</td></tr>
<tr><td><b>Step out</b></td><td>Run to the current function's return.</td></tr>
<tr><td><b>Reset</b></td><td>Reset the CPU and peripherals.</td></tr>
</table>
<p>Controls are disabled when they cannot apply (for example Step while running).</p>
<h3>Running versus stopped</h3>
<p>While running, the CPU, Disassembly and Source panes follow a sampled snapshot of the
PC, and the peripheral docks refresh as the simulation advances. Panes that read memory
with side effects or edit state (writing a register, the Memory view, the Watch pane
values) work on a <b>stopped</b> simulator; pause first. The marker moves in refresh
steps (about 20 per second), so at full speed it shows a sampled position, not every
instruction.</p>
<p><b>Speed:</b> the <code>--speed</code> flag sets throttling; 1.0 matches simulated time to
wall time, 0 runs as fast as possible.</p>
"""),
    ("Pin panel", """
<h2>Central pin panel</h2>
<p>One cell per pin showing its level and direction.</p>
<ul>
<li><code>OUT 1</code> / <code>OUT 0</code>: the pin is an output driven by the firmware.</li>
<li><code>Z 0</code> / <code>Z 1</code>: an input (high impedance) and the level seen on it.</li>
<li>Tick <b>drive</b> to drive an input pin from outside, and <b>H</b> to choose the level
(checked = high). Driven pins are saved in sessions.</li>
</ul>
<p>Use this to simulate a button or an external signal without a Board.</p>
"""),
    ("Board", """
<h2>Board dock</h2>
<p>Virtual parts wired to pins: LEDs, push buttons (latching or momentary), a rotary
encoder, an HD44780 LCD and a 128x64 SSD1306 OLED (I2C or 4-wire SPI).</p>
<p>Load one from <b>Board&hellip;</b> or <i>Session &rarr; Open board&hellip;</i>. Loading a board
applies its MCU and clock, so load it before the firmware (the command line does this).</p>
<h3>Shipped boards (in <code>boards/</code>)</h3>
<ul>
<li><code>demo_4809</code>, <code>demo_4808</code>, <code>demo_3217</code>, <code>demo_1614</code>,
<code>demo_212</code>: generic LED/button/encoder/LCD layouts (only demo_4809 has matching firmware).</li>
<li><code>oled_4809</code>: SSD1306 on TWI0 at 0x3C.</li>
<li><code>curiosity_nano_4809</code>, <code>curiosity_nano_3217</code>: the Curiosity Nano LED0 and SW0.
Neither has an external pull-up on SW0, so firmware must enable the internal one.
Matching firmware: <code>examples/demo_3217_nano</code> and <code>examples/demo_4809_nano</code>.</li>
</ul>
<h3>Writing your own</h3>
<p>JSON or TOML; see <code>boards/demo_4809.json</code>. An OLED is
<code>{"type": "oled", "bus": "twi", "addr": 60}</code> or
<code>{"type": "oled", "bus": "spi", "dc": "PC1", "cs": "PC0", "res": "PC2"}</code>.
The bus pins are the firmware's; <code>cs</code>/<code>dc</code>/<code>res</code> are GPIOs the
panel listens to.</p>
<p>Parts flow left to right in a 4-column grid. Give a part both <code>row</code> and
<code>col</code> (0-based) to pin it to a cell instead, e.g.
<code>{"type": "led", "pin": "PA0", "row": 1, "col": 2}</code>; two parts may not share a cell.
A bad board shows a warning and is not loaded.</p>
"""),
    ("Debugging", """
<h2>Debugging</h2>
<h3>Disassembly</h3>
<p>Instructions around the PC with symbols and source lines. The PC row is highlighted and
follows the PC while running. Double-click a row to toggle a breakpoint; right-click for
<b>Run to here</b> or <b>Toggle breakpoint</b>.</p>
<h3>Source</h3>
<p>The C source around the current line (needs <code>-g</code>). Double-click a line to toggle
a PC breakpoint at its first instruction; right-click for <b>Run to line</b>. Lines with a
breakpoint show a dot.</p>
<h3>Breakpoints</h3>
<p>Enter a hex address or a symbol (<code>main</code>, <code>main+0x4</code>) and press Enter.
Prefix <code>0x</code> to force an address over a symbol. The breakpoint that stopped the run is
highlighted.</p>
<h3>Data Breakpoints (watchpoints)</h3>
<p>Syntax <code>hexaddr[:len] [mode]</code>, for example <code>0x0400</code> or <code>3f:2 rw</code>.
Hex is always hex; <code>len</code> is decimal. Modes:</p>
<ul>
<li><code>r</code> read, <code>w</code> write (default), <code>rw</code> either;</li>
<li><code>c</code> a CPU write that changes the value;</li>
<li><code>p</code> any change by anyone (CPU or peripheral model).</li>
</ul>
<p>r/w/rw/c see CPU accesses only. The one that triggered is highlighted when the run stops.</p>
<h3>Watch</h3>
<p><code>[label=]hexaddr[:width]</code>, e.g. <code>porta_out=0x0405</code>. Shows live data-space values.</p>
<h3>Stepping</h3>
<p><b>Step</b> = one instruction. <b>Next line</b> steps over a source line or call, using a
temporary breakpoint at the return. <b>Step out</b> runs to the function's return. Both need debug
info. <b>Run to here</b> / <b>Run to line</b> use a temporary breakpoint that is removed when hit.</p>
<h3>CPU</h3>
<p>State, cycle, time, PC/SP/SREG (with flags as <code>ITHSVNZC</code>) and r0&ndash;r31.</p>
"""),
    ("Interrupts", """
<h2>Interrupts</h2>
<p>Per-vector count, CPUINT level and last cycle, with vector names. Also shown:</p>
<ul>
<li><b>executing</b>: which CPUINT level is currently running its handler.</li>
<li><b>pending</b>: vectors raised and enabled but not yet taken, in dispatch order
(NMI, then LVL1, then LVL0 by priority). Whether one is taken next depends on the I flag
and the CPUINT level.</li>
<li><b>flags set</b>: peripherals whose INTFLAGS register is non-zero while stopped. A flag only raises an
interrupt if its enable bit, the global I flag and the level allow it.</li>
</ul>
<p><b>Clear</b> resets the counters.</p>
"""),
    ("Peripheral docks", """
<h2>Peripheral docks</h2>
<p>Register views decoded from the device's own header. Most are display-only.</p>
<ul>
<li><b>Registers</b>: every register with address, hex value and binary. Use the filter box
(<code>TCA0</code>, <code>CTRLA</code>, <code>porta.out</code>). Values that changed since the last refresh are
highlighted. The <b>Value</b> column is editable (hex): pause, edit, press Enter. The write has IO side
effects like a CPU write.</li>
<li><b>Ports</b>: per pin direction, OUT latch, input level, invert, pull-up and sense config.
Changed levels are highlighted.</li>
<li><b>PORTMUX</b>: each routing field, its value, the selected option and the pins. Bold = not the reset
routing. On the ATtiny3216/3217, USART0 TXD and TCA0 WO0&ndash;WO5 follow these fields: the signal shows on the
selected pin (Ports, pin panel, Timeline, VCD) and moves when you change the field. Other fields are shown only.</li>
<li><b>Timers</b>: TCA (single/split), TCB, TCD registers one row per field. The Timeline draws outputs.</li>
<li><b>Events</b>: Event System channels, generators and users, with names from the device header.</li>
<li><b>Analog</b>: ADC, VREF, AC and DAC state with derived values (reference level, ADC result in volts, AC
divider). Where a reference is VDD, set the <b>VDD</b> box (the simulator has no supply register).</li>
<li><b>Serial</b>: USART, SPI and TWI state. Reading has no side effects, so RXDATA is not consumed.
USART baud rate is derived from BAUD assuming CLK_PER equals the simulated CPU clock.</li>
<li><b>Clock</b>: CLKCTRL state with the derived main clock source, prescaler, CLK_PER and CLK_MAIN.</li>
<li><b>System</b>: RSTCTRL, SLPCTRL, BOD, WDT, CPUINT, CRCSCAN, RTC/PIT, CCL, NVMCTRL and SYSCFG for the
blocks the device has.</li>
</ul>
<p>Docks only list blocks the selected device actually has.</p>
"""),
    ("Timeline", """
<h2>Timeline</h2>
<p>A logic-analyser view over simulated time.</p>
<ul>
<li>Type a pin (<code>PA0</code>), a timer output (<code>TCA0.WO1</code>; <code>WO3</code>&ndash;<code>WO5</code> are split-mode high-byte outputs) or a bus (<code>SPI0</code>,
<code>TWI0</code>, <code>USART1</code>) and press Enter to add a trace. UART0 TX and interrupt vectors are
also shown.</li>
<li><b>Mouse wheel</b> zooms; <b>drag</b> pans.</li>
<li><b>Click</b> sets cursor A, <b>right-click</b> sets cursor B; the info line shows the measurement between them.</li>
<li><b>Double-click</b> a trace label to remove it.</li>
<li><b>Follow</b> keeps the newest activity in view. <b>Clear</b> empties the history.</li>
<li><b>Export VCD&hellip;</b> writes a GTKWave-compatible file.</li>
<li><b>Export A&ndash;B&hellip;</b> writes only the cycles between the two cursors.</li>
</ul>
"""),
    ("Memory & UART", """
<h2>Memory</h2>
<p>A hex view of <b>data</b>, <b>flash</b> or <b>eeprom</b>. Choose the space with the buttons, enter an address
and a length (16&ndash;4096), and the view updates. It is read-only.</p>
<h2>UART</h2>
<p>Console for USART0. Bytes the firmware transmits appear in the output (non-printable bytes as
<code>\\xNN</code>; history is capped). Type a line and press Enter to send it to the firmware's USART0 receiver.</p>
"""),
    ("Sessions & settings", """
<h2>Sessions &amp; settings</h2>
<p><i>Session</i> menu: <b>Open board&hellip;</b>, <b>Save session&hellip;</b>, <b>Load session&hellip;</b>.</p>
<p>A session file (JSON) stores firmware, MCU, clock, speed, board, breakpoints, data breakpoints, watches,
driven pins, timeline traces and the window layout. Older session versions still load.</p>
<p>The window layout, MCU, clock and last firmware folder are also remembered automatically between launches in
<code>~/.config/simavr/simavr-gui.conf</code>. Explicit command-line flags win.</p>
<h3>Layout</h3>
<p>Docks can be dragged, tabbed, floated and closed. Default: the right column holds CPU plus a tab group of debugger panes;
the left holds a tab group of peripheral panes; the bottom holds Timeline plus Memory/UART.</p>
"""),
    ("Limits & tips", """
<h2>Limits &amp; tips</h2>
<ul>
<li>Peripheral docks are register views, not events in flight.</li>
<li>PORTMUX re-routes only USART0 TXD and TCA0 WO0&ndash;WO5 on the ATtiny3216/3217 so far; the firmware must still set the pin's DIR as an output.</li>
<li>A core missing from the MCU box means its device header did not preprocess with the installed toolchain.</li>
<li>No source lines or symbols? Rebuild with <code>-g</code> and make sure the ELF (not a HEX) is loaded.</li>
<li>Edits to registers or memory need a stopped simulator.</li>
<li>Details and per-feature notes: <code>doc/python_gui_todo.md</code>; Python API: <code>python/README.md</code>.</li>
</ul>
"""),
]


class HelpDialog(QDialog):
    """Non-modal help browser: topic list, search box, and the page."""

    def __init__(self, parent=None, topic: str | None = None):
        super().__init__(parent)
        self.setWindowTitle("simavr GUI Help")
        self.resize(900, 650)
        self.setWindowFlag(Qt.WindowType.WindowMinMaxButtonsHint, True)
        self._list = QListWidget()
        self._list.setMaximumWidth(220)
        self._list.addItems([t for t, _ in TOPICS])
        self._search = QLineEdit()
        self._search.setPlaceholderText("Find in page (Enter)")
        self._search.returnPressed.connect(self._find)
        self._view = QTextBrowser()
        self._view.setOpenExternalLinks(True)
        left = QVBoxLayout()
        left.addWidget(self._list)
        right = QVBoxLayout()
        right.addWidget(self._search)
        right.addWidget(self._view)
        lay = QHBoxLayout(self)
        lay.addLayout(left)
        lay.addLayout(right, 1)
        self._list.currentRowChanged.connect(self._show)
        self._list.setCurrentRow(self.index_of(topic))

    @staticmethod
    def index_of(topic: str | None) -> int:
        return next((i for i, (t, _) in enumerate(TOPICS) if t == topic), 0)

    def _show(self, row: int):
        if 0 <= row < len(TOPICS):
            self._view.setHtml(TOPICS[row][1])

    def _find(self):
        text = self._search.text()
        if text and not self._view.find(text):
            self._view.moveCursor(self._view.textCursor().MoveOperation.Start)
            self._view.find(text)       # wrap around
