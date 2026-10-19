# Python GUI simulator TODO

Separate roadmap for a Python-first GUI frontend for modern AVR simulation.
This is intentionally distinct from `modern_support_todo.md`, which tracks
simulation-core fidelity gaps in the C engine. The assumption here is:

* keep the MCU execution + peripheral models in native code
* expose a stable host/control API
* build the interactive GUI in Python

Headless support is optional; GUI usability is the priority.

Verified against the current tree on 2026-06-08.

## Goals

* Replace the ad-hoc GLUT/OpenGL example-board UI style with a richer desktop UI
  that is easier to extend.
* Make modern AVR boards easier to inspect visually: pins, registers, buses,
  breakpoints, framebuffers, virtual LEDs, buttons, and serial consoles.
* Keep simulator correctness work in the native core; do not re-implement AVRxt
  execution or peripheral timing in Python.

## Recommended shape

* **Core:** existing `simavr/sim` engine remains native.
* **Bindings/API:** add a narrow host-facing API for lifecycle, stepping,
  breakpoints, memory/register access, IRQ wiring, and trace subscription.
* **GUI toolkit:** prefer **PySide6 / Qt** over Tkinter, GLUT, or browser-first
  shells.
* **Board models:** Python-side composition of virtual widgets around the native
  MCU instance.

## Phase 1 — host API extraction

**Status (2026-10-18): first cut landed** as
[`simavr/sim/sim_host.h`](../simavr/sim/sim_host.h) /
[`sim_host.c`](../simavr/sim/sim_host.c), covered by the host-only
[`tests/test_sim_host.c`](../tests/test_sim_host.c). Decisions taken:

* one library-owned sim thread per instance; an instance lock serialises all
  `avr_t` access (accessors wait at most one batch), and `pause` is lock-free
* events go into a bounded queue drained by `avr_host_poll_events()`, with a
  pollable fd; no callbacks into the GUI thread (avoids GIL / foreign-thread
  issues for Python), per-pin coalescing for fast signals
* only the `avr_host_*` functions and the public `AVR_IOCTL_*` IRQ ids are the
  contract; `avr_host_raw_avr()` is an explicitly unstable escape hatch for
  existing C parts

Implemented: lifecycle, ELF/HEX/raw load (incl. modern EEPROM records),
run/pause/step/run-cycles, speed throttle, breakpoints, memory + CPU access,
pins by name with external drive, USART TX/RX, pin/USART/raw-IRQ events.

Still `AVR_HOST_ENOTSUP`: register/peripheral metadata table (needs a per-core
table generated from the avr-libc headers), ELF symbols, watchpoints, analog
pin voltages, interrupt-taken events, log text, VCD. Open item: pin down AVRxt
single-step semantics (multi-cycle instructions, CPUINT level-1 preemption).

1. Define a stable C API for:
   * create/destroy MCU instances
   * load ELF / HEX
   * run / pause / single-step / reset
   * register + SRAM + flash reads/writes
   * subscribe to pin/IRQ/bus events
   * inject GPIO/input events from the GUI
2. Stop depending on example-board host code as the primary integration surface.
   The current `examples/parts` and board runners are useful references, but not
   a clean API boundary.
3. Decide event transport:
   * callback-based C API for low-latency UI updates
   * optional polling snapshots for simpler widgets
4. Mark which existing internals are private so bindings do not couple to
   unstable structures.

## Phase 2 — Python bindings

**Status (2026-10-18): first cut landed** as [`python/`](../python)
(`simavr-py`, package `simavr_py`). Decisions taken:

* cffi in ABI mode (`ffi.dlopen` of `libsimavr.so`): no compiler at install
  time; the hand-written cdef is checked against `sim_host.h` (function list)
  and against `cc` (struct layouts) by the tests, so drift fails loudly
* targets the project's Python 3.15 env only (`requires-python >= 3.15`) and
  uses its features: lazy imports (cheap `import simavr_py`), `frozendict`
  (pin and error tables), `type` aliases, `match` event decoding
* free-threaded 3.15t verified: the suite passes and the GIL stays disabled
  after cffi + libsimavr load; calls run GIL-free and rely on the C lock
* events surface as typed frozen dataclasses (`PinEvent`, `UartTxEvent`,
  `StateEvent`, ...); `Simulator.fileno()` plugs into `select` /
  `QSocketNotifier`

Not wrapped yet: the C calls that still return `AVR_HOST_ENOTSUP`.

1. Choose the binding layer:
   * `cffi` for fast iteration and a plain C ABI
   * `pybind11` only if the host API becomes C++
   * avoid `ctypes` as the long-term primary interface
2. Build a small Python package layout:
   * `simavr_py.core`
   * `simavr_py.bindings`
   * `simavr_py.widgets`
   * `simavr_py.boards`
3. Provide Python wrappers for:
   * MCU session lifecycle
   * stepping/clock control
   * register file and named peripheral views
   * trace subscriptions
   * bus attachments for UART/SPI/TWI-backed virtual devices
4. Add smoke tests that load a known modern target such as `attiny3217` or
   `atmega4809` and verify basic control/readback from Python.

## Phase 3 — GUI shell

**Status (2026-10-18): first cut landed** as `simavr_py.gui`
(`python -m simavr_py.gui [-m MCU] [-f HZ] [--speed R] firmware.elf`, extra
`gui` = PySide6). Done: run/pause/step/reset toolbar, pin panel (level, direction,
click-to-drive), CPU state view, memory viewer (data/flash/EEPROM), USART0
console with TX input. Decisions: repaint on a fixed 50 ms `QTimer`, independent
of simulated time; events via `QSocketNotifier` on `Simulator.fileno()`, drained
in bounded batches so a chatty firmware can't starve the event loop; the C API
only exposes PC/SP/SREG/r[] while paused, so the CPU view keeps its last
snapshot while running. Added later the same day: watch pane (data-space `[label=]hexaddr[:width]`,
changed-value highlight) and session save/restore (Session menu or
`--session FILE.json`: firmware, MCU, clock, window/dock layout, watches, driven
pins). Not done: board canvas, per-pin edge coalescing/timeline, named
register watches (blocked on the register metadata table). Known quirk: on 3.15.0a7,
importing PySide6 stops `simavr_py.<name>` attribute access from reifying lazy
imports (use `from simavr_py.core import ...` alongside PySide6).

1. Build a Qt desktop shell with:
   * board canvas
   * register inspector
   * memory viewer
   * UART console
   * trace/watch pane
   * run/pause/step toolbar
2. Separate simulation time from repaint cadence so the GUI stays responsive
   under high simulated clock rates.
3. Add a signal/slot bridge that coalesces high-rate pin changes instead of
   repainting every transition.
4. Support session save/restore for window layout, watched registers, and board
   wiring.

## Phase 4 — modern-board widgets

**Status (2026-10-18): slice 1 landed.** `simavr_py.boards` (declarative
JSON/TOML board files, strict validation, `BoardRuntime` binding a board to a
`Simulator`), `simavr_py.widgets` (Qt-free LED / button / rotary-encoder
models) and a "Board" dock in the GUI (`--board FILE`, Session > Open board,
saved in sessions). Example: `python/boards/demo_4809.json`. Decisions: LED
brightness is the lit fraction of simulated time between repaints, from pin
events coalesced to ~0.5 ms, so PWM ramps read as ramps; released buttons let
go of the pin (firmware pull-up decides) unless `pull` models an external
resistor; an encoder detent plays its four quadrature phases 3 ms apart in
wall-clock time. **Slice 2 (2026-10-18): HD44780 LCD** (`lcd` board widget,
`widgets.LcdModel`, `LcdWidget`). A pure-Python decoder over uncoalesced pin
events: latches on E's falling edge, 4- and 8-bit buses (a 4-bit wiring powers
up in 8-bit mode with D0-D3 floating, so the standard `3,3,3,2` start-up
works), DDRAM/CGRAM, entry mode, display/cursor shift, blink, 1/2/4 rows. Write
only: no busy flag or read-back (firmware must use fixed delays) and no
execution timing. Pin-queue overflow marks the display "events lost" until the
next clear. Not done: SSD1306/SH1106 (need a bus-attach in the host API or a
Python SPI/TWI decoder), and the inspection widgets (blocked on the register
metadata table).

1. Recreate the current visual demos in Python:
   * LED ramp
   * SSD1306 / SH1106 OLED
   * HD44780 LCD
   * buttons / rotary encoder
2. Add modern-AVR-specific inspection widgets:
   * PORT/VPORT live state by physical pin
   * PORTMUX routing view
   * TCA/TCB/TCD waveform state
   * EVSYS channel/user routing
   * ADC/VREF/AC analog state summaries
3. Make widget wiring declarative so a board definition is mostly data, not
   custom glue code.

## Phase 5 — debugger-quality UX

**Status (2026-10-18): PC breakpoints landed.** A "Breakpoints" dock
(`BreakpointPane`) lists flash byte addresses, arms them on every (re)loaded
simulator, highlights the one that stopped the run, and saves them in the
session (format version 3). Addresses beyond the device's flash stay listed but
show unarmed. Data watchpoints are blocked on the native `avr_host_watch_add`
(not implemented); disassembly landed (below); source correlation is not started.

**Disassembly (slice 2, 2026-10-19):** `simavr_py.disasm` (Qt-free) decodes the
AVRe+/AVRxt set and is checked opcode-for-opcode against `avr-objdump` (all
65,536 words; XCH/LAS/LAC/LAT/DES stay `.word` because no core implements them).
The "Disassembly" dock shows instructions around the PC while stopped, reading
flash on each stop (so SPM-modified code shows correctly); double-click a row to
toggle a PC breakpoint. Rows before the PC are best-effort (variable-length code
can't be decoded backwards; the decoder resyncs onto the PC), rows from the PC
on are exact. Branch/call targets are shown as absolute addresses; no symbol
names yet (that needs the ELF symbol table, with source correlation).

**Symbols (slice 3, 2026-10-19):** `simavr_py.symbols` (Qt-free, no
dependencies) reads the ELF `.symtab` for code symbols (FUNC/untyped labels in
executable sections; `.L*` locals and debug labels dropped). The disassembly
dock gains a "Symbol" column (function start) and annotates branch/call targets
as `; name+0xN`. HEX loads have no symbols. Not done: DWARF line numbers /
source view.

**Symbol breakpoints (slice 4, 2026-10-19):** the breakpoint box accepts an ELF
symbol (`main`) or `name+hexoffset`; an exact symbol name wins over a bare hex
reading (`0xadd` forces the address). The pane still stores plain addresses, so
sessions are unchanged.

**Interrupt activity (slice 5, 2026-10-19):** `avr_host_subscribe_interrupts` is
now implemented (it returned ENOTSUP): one `AVR_HOST_EV_INTERRUPT` per vector
*taken* (a=vector, b=CPUINT level 0/1/2; RETIs are filtered out), hooked on the
interrupt table's RUNNING IRQ. Python: `Simulator.subscribe_interrupts()` /
`InterruptEvent`; GUI: an "Interrupts" dock with per-vector level, dispatch
count and last cycle (Clear button; resets on load). Not done: pending/enabled
state, vector names (blocked on the register metadata table), a timeline, and
test coverage of LVL1/NMI levels and nested-return filtering (only LVL0 is
exercised).

**Data breakpoints (slice 6, 2026-10-19):** `avr_host_watch_add/remove` are
implemented as pausing watches over a data-space range (READ / WRITE /
WRITE|CHANGE), via a new `avr->cpu_watch` hook called from the CPU's
`_avr_get_ram/_avr_set_ram` (one NULL check when unused). CPU accesses only:
peripheral-model writes and host peeks/pokes are not seen, so this is not yet a
"peripheral register changed" trigger. The stop lands after the accessing
instruction (STOP_WATCHPOINT, id). Non-pausing `AVR_HOST_EV_WATCH` events are
not produced. Python: `Simulator.add_watchpoint/remove_watchpoint`, `WatchFlag`;
GUI: a "Data Breakpoints" dock (`hexaddr[:len] [r|w|rw|c]`), saved in the
session (format version 4).

**Source lines (slice 7, 2026-10-19):** `simavr_py.lines` (Qt-free) reads
`.debug_line` (DWARF 2-5, 32-bit DWARF) from the ELF; the disassembly dock gains a
"Source" column (`file.c:line`, full path in the tooltip). Checked against
`avr-objdump --dwarf=decodedline` for each DWARF version. At an address shared by
several rows the last row wins. Not done: source-level stepping, inlined-call
info (`.debug_info`), compressed or 64-bit DWARF.

**Source view (slice 8, 2026-10-19):** a "Source" dock shows the source file
(read from the path recorded in the ELF, +/-200 lines) with the PC's line
highlighted, refreshed on each stop; it reports "no source line" for code without
debug info and "file not found" if the path is gone. Read-only: no breakpoints
by clicking a source line yet, no step-over-line.

1. Breakpoints:
   * PC breakpoints
   * data watchpoints (CPU accesses done; peripheral-register triggers not)
   * peripheral-register change triggers
2. Add disassembly and source correlation from ELF/DWARF where available.
3. Show interrupt activity:
   * pending/active sources
   * vector taken
   * CPUINT state for modern AVR
4. Add timeline views for selected pins, timers, and bus traffic without forcing
   the user into VCD/GTKWave for every task.

## Native-core prerequisites

These are not Python tasks, but they strongly affect GUI usefulness:

* pin-level peripheral routing through `PORTMUX` instead of only dedicated IRQ
  endpoints
* better line-level USART/SPI visibility
* stable hooks for trace/event subscription
* documented stepping semantics for AVRxt timing and interrupt delivery
* a supported way to query named registers/peripherals without scraping internal
  structs

## Non-goals

* Rewriting the AVR execution engine in Python
* Making headless/CI the primary design center
* Building a web app before the native desktop API is stable

## Suggested work order

1. Define the native host API.
2. Prove it with a minimal Python binding and one modern target.
3. Build a Qt shell with run/pause/step, UART, and GPIO views.
4. Port one existing board demo end-to-end, preferably `board_atmega4809_oled`
   or `board_attiny3217_leds`.
5. Add richer inspector widgets only after the API boundary holds.

## Open questions

* Should the Python layer own board composition entirely, or should some virtual
  parts remain native for reuse across C and Python hosts?
* Is the first-class abstraction an MCU, a board, or a simulation session
  containing multiple devices?
* Should waveform capture be a native service exposed to Python, or a pure
  Python consumer over trace callbacks?
