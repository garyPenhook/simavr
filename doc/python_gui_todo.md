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

Also implemented since: data/peripheral watchpoints and interrupt-taken events.
Still `AVR_HOST_ENOTSUP`: register/peripheral metadata table (needs a per-core
table generated from the avr-libc headers), ELF symbols (the Python side reads
the ELF itself: `simavr_py.symbols`/`lines`), analog pin voltages, log text, VCD
(the GUI timeline writes its own VCD). Open item: pin down AVRxt
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
next clear. **Slice 3 (2026-10-19): SSD1306 OLED** (`oled` board widget). A Python decoder can't
ACK a TWI byte in time (events are asynchronous), so the controller is a native
part, `simavr/sim/avr_ssd1306.c`, wired to a TWI or 4-wire SPI bus plus CS/DC/RES
pins; the host API gains `avr_host_oled_attach/read/detach` (up to four per
instance, slots reused) and the GUI draws the snapshot (`widgets.oled_pixels`,
`OledWidget`). Behaviour follows the SSD1306 Rev 1.0 data sheet: reset into page
addressing, horizontal/vertical/page modes with column/page windows, display
on/off, entire-on, inverse, contrast, start line, segment remap, COM scan flip.
Not modelled: scrolling (commands accepted, ignored), display offset D3h,
multiplex ratio, timing. Unlike `examples/parts/ssd1306_virt.c` it never prints
or aborts. Tests run the real `board_atmega4809_oled` firmware over I2C and SPI
(4160 lit pixels, as the C example's check) and the widget in the GUI. SH1106 and
two OLEDs on one bus are untested; the inspection widgets stay blocked on the
register metadata table (the Ports dock covers PORT state).

**PORTMUX view (2026-10-19):** a "PORTMUX" dock decodes every routing field of the
device (`simavr_py.portmux.routes`, Qt-free) from the avr-libc header's
`PORTMUX_<field>_<option>_gc` enums (so a smaller package lists fewer options),
showing register value, option name and pin text. Bit positions and reset values
were checked against DS40002173C (megaAVR-0) and DS40002205A/04A/287A (tinyAVR
0/1); tinyAVR 0/1 `EVOUTn` bits are enables there, not pin selects, and are shown
as OFF/ON. Limits: tinyAVR 0/1 headers give no pin lists ("Default pins"); it
is a display only, because the engine stores PORTMUX without re-routing. The work
found and fixed an engine gap: megaAVR-0 registered 4 of its 6 PORTMUX registers
(TCAROUTEA/TCBROUTEA weren't reset); covered in `test_avrxt_engine.c`.

**Timer view (2026-10-19):** a "Timers" dock decodes TCA (single and split), TCB and TCD
registers (`simavr_py.timers`, Qt-free) from the avr-libc header's `TC*_gm/_bm` masks and
`_gc` enums; no behaviour is asserted beyond what the header names. Read while stopped,
changed values highlighted. Limits: TCD WGMODE/CNTPRES show a number when the header has no
enum; it is a register view, not a waveform (Timeline lanes cover outputs).

**Event System view (2026-10-19):** an "Events" dock (`simavr_py.evsys`, Qt-free) shows each
channel's generator and the channel each user listens to. Two layouts, found from the
register names: `CHANNELn` + `USER*` (user value 0 = OFF, n = CHANNEL(n-1)) and tinyAVR 0/1
`ASYNCCHn`/`SYNCCHn` + `ASYNCUSERn`/`SYNCUSERn` (1-2 = SYNCCH0-1, 3-6 = ASYNCCH0-3). Verified
against DS40002173C 14.5.2/14.5.3 (megaAVR-0) and DS40002205A 14.5.5/14.5.6 (tinyAVR 1);
the tinyAVR 2-series mapping follows its header's `EVSYS_USER_CHANNELn` enum and is **not**
checked against a 2-series data sheet (none in `datasheets/`). Generator and user names come from
the header, a user value that selects nothing shows `?`. Display only; no event activity.

**Analog view (2026-10-19):** an "Analog" dock (`simavr_py.analog`, Qt-free) shows one row per
ADC / VREF / AC / DAC register with its value and every field the header defines for it. Fields
are the `_gm`/`_bm` masks under each `/* ADC.CTRLA  bit masks ... */` banner (a register that
reuses another's bits, like ADC INTFLAGS, is `/* ADC_RESRDY is already defined. */` and is
resolved to the first definition); option names are the `_gc` enum entries, a value with no enum
entry shows as a number. Derived rows, each from the data sheet: VREF levels (the option name
carries the voltage; the header's `4V34` is **4.3 V** in DS40002173C/205A/204A, 4.34 V is only a
BOD level); ADC result in volts (megaAVR-0/tinyAVR 0/1: RES = 1023 x VIN / VREF, DS40002173C
29.3.2.5, averaged over SAMPNUM, 8-bit = top 8 bits; tinyAVR 2-series: SAMPLE per DS40002345B
30.3.3.3, 12/8-bit, differential, PGA gain only when PGAEN and VIA=PGA, LEFTADJ); AC DACREF
output (DACREF x VREF / 256). Mux options name their pad from `analog_pins.json`, generated from
the ATDF device files (`python/tools/gen_analog_pins.py`; 45 of 46 devices, ATtiny3214 has no
ATDF). VDD references use a VDD spin box (the simulator has no supply register); an external
VREFA has no known voltage so no result. Limits: only the latest SAMPLE (2-series) or RES is
converted, never the 32-bit accumulated RESULT; DAC output volts and the temperature sensor are
not converted. **Open engine question:** `avr_adc_modern.c` converts with `Vin * 1024 / VREF`
(clamped) while the data sheet says `1023 * VIN / VREF`, so a simulated input can read up to one
count off the dock's volts.

**Serial view (2026-10-19):** a "Serial" dock (`simavr_py.comms`, Qt-free, reuses the Analog field decoder) shows USART, SPI and TWI registers with every header-defined field, plus a USART **Baud rate** row (`64 x fCLK_PER / (S x BAUD)`, S = 16 / 8 with RXMODE=CLK2X; synchronous `fCLK_PER / (2 x BAUD[15:6])`; DS40002173C Table 23-1). Limits: CLK_PER is taken as the simulated CPU clock (the CLKCTRL prescaler is not read); no TWI/SPI derived rates; the TWI baud formula needs rise time and was not added. Display only.

**Clock view (2026-10-19):** a "Clock" dock (`simavr_py.clock`, Qt-free, reuses the Analog field decoder) shows the CLKCTRL registers with every header-defined field, plus Main clock (CLKSEL), Prescaler (PEN/PDIV; PDIV ignored with PEN=0, reserved codes shown as reserved) and CLK_PER / CLK_MAIN (DS40002205A 10.3, 10.5.1-10.5.2). CLK_PER is the simulated CPU clock, which the engine takes from CLKCTRL, so CLK_MAIN is CLK_PER x division, not a frequency table. Same register layout on all 46 cores. Limits: oscillator control, calibration and MCLKSTATUS are shown only as header fields; display only.

**System view (2026-10-19):** a "System" dock (`simavr_py.system`, Qt-free, reuses the Analog field decoder) shows RSTCTRL, SLPCTRL, BOD, WDT, CPUINT, CRCSCAN, RTC (incl. PIT), CCL, NVMCTRL and SYSCFG registers with every header-defined field, for whichever of them the device has. No derived rows (no timeout or period conversion). Display only.

1. Recreate the current visual demos in Python:
   * LED ramp
   * SSD1306 / SH1106 OLED
   * HD44780 LCD
   * buttons / rotary encoder
2. Add modern-AVR-specific inspection widgets:
   * PORT/VPORT live state by physical pin
   * PORTMUX routing view
   * TCA/TCB/TCD waveform state (register view done; waveform = Timeline lanes)
   * EVSYS channel/user routing (done)
   * ADC/VREF/AC analog state summaries (register view done; DAC included)
3. Make widget wiring declarative so a board definition is mostly data, not
   custom glue code.

## Phase 5 — debugger-quality UX

**Status (2026-10-18): PC breakpoints landed.** A "Breakpoints" dock
(`BreakpointPane`) lists flash byte addresses, arms them on every (re)loaded
simulator, highlights the one that stopped the run, and saves them in the
session (format version 3). Addresses beyond the device's flash stay listed but
show unarmed. Later slices (below) added data watchpoints (slice 6, native
`avr_host_watch_add` is implemented), disassembly, symbols, source correlation,
stepping, interrupts and the pin timeline.

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
debug info and "file not found" if the path is gone. Read-only apart from the breakpoint toggle below.

**Source breakpoints (slice 9, 2026-10-19):** double-clicking a line in the Source
dock toggles a PC breakpoint at the line's lowest address (`LineTable.address_of`;
a line with no code resolves to the next later line that has some). Lines whose
address holds a breakpoint show a ● marker. Only lines near the PC are shown, so
breakpoints elsewhere in a file still need the address/symbol box.

**Step over line (slice 10, 2026-10-19):** a "Next line" toolbar action steps
until the source line changes, running over `call/rcall/icall/eicall` with a
temporary breakpoint at the return address. It is driven by stop events from the
GUI thread (never blocks), so Pause (enabled while it works) cancels a line that
never ends (`for(;;);`); Run, Step, Reset, a user breakpoint, a watchpoint or
program exit also cancel it. Without line info it is a plain Step. Limits: a
line that re-enters itself by recursion can stop in the inner frame (the return
breakpoint is not frame-checked); an interrupt taken mid-line stops in the ISR if
it has line info.

**Step out (slice 11, 2026-10-19):** a "Step out" action reuses the same machine:
it steps (over calls) until a RET/RETI executes with SP above its value at the
start, i.e. the current function's own return, and stops just after it in the
caller. No debug info needed. An ISR taken meanwhile is stepped through and its
RETI doesn't count (it restores SP to at most the starting value). Step *into* is
just Step. The "running" state event produced by our own resume is now ignored
rather than treated as a foreign stop (it could cancel a step over a call).

**Peripheral-register triggers (slice 12, 2026-10-19):** `AVR_HOST_WATCH_PERIPH`
(Python `WatchFlag.PERIPH`, GUI mode `p`) pauses when any byte of the range
differs after an instruction, whoever wrote it. It is a snapshot compare after
each `avr_run()` (only while a PERIPH watch exists), so no peripheral model needs
hooking; it also catches changes made while asleep. The baseline is re-taken at
every resume, so host pokes never fire it. Limits: granularity is one
instruction/cycle-timer step, a value that changes and changes back within one
step is missed, and the stop does not say who wrote it. SREG is not covered (the
core keeps it outside `data[]`).

**Pin timeline (slice 13, 2026-10-19):** a "Timeline" dock draws digital
waveforms of chosen pins (type `PA0` + Enter; double-click a label to remove).
`simavr_py.timeline.PinTrace` (Qt-free) keeps each pin's edges (200k cap, oldest
trimmed); the dock uses its own `subscribe_pin` subscriptions, routed by
subscription id so the board's LED/LCD models never see the extra edges. Span
10 µs-1 s, wheel zooms, drag pans, "Follow" tracks the newest cycle, Clear
restarts the traces. Pins are saved in the session (format version 5). Limits:
a very busy pin at unthrottled speed overflows the event queue; the trace then
resyncs to the pin's *current* level (approximate) and follow-mode can lag the
simulator until the queue drains.

**Timeline extras (slice 14, 2026-10-19):** click = cursor A, right-click =
cursor B; the status line shows both times, Δ and 1/Δ (`measure`). A "UART0 TX"
lane marks each byte USART0 sent (labelled when there is room; the cycle is when
the byte left the simulated shifter). "Export VCD…" writes the traced pins plus
the UART lane (`uart0_tx`, 8-bit) at 1 ns resolution (`write_vcd`), for GTKWave.
Not done: analog values, exporting a cycle range only (the export is the whole
retained history).

**Timer lanes (slice 15, 2026-10-19):** the pin box also takes timer waveform
outputs: `TCA0.WO0-2`, `TCB0-3.WO`, `TCD0.WOA/WOB`. They subscribe to the timer's
output IRQ (`subscribe_irq`, same ioctl/index the engine test uses), so they show
the timer's own output even when PORTMUX doesn't route it to a pin; they are
saved in the session with the pins. The initial level is assumed low (an IRQ's
value can't be read back). A new "IRQ" lane marks every interrupt vector taken
(vector number; no names yet), which is where timer overflow/compare/capture
interrupts appear, and is included in the VCD (`irq_vector`). Tests check the
name mapping, that the subscriptions are accepted/rejected per device, and the
drawing; they do not drive a firmware-configured timer end to end.

**SPI and TWI lanes (slice 16, 2026-10-19):** `SPI0` / `TWI0` in the pin box add
two marker lanes each, `out` (what this MCU puts on the wire) and `in` (what a
connected part sends back), from the peripheral's wire IRQs (the ones attached
parts use: SPI input/output, TWI input/output). SPI labels are the byte; TWI
labels decode simavr's message word (`S A0` start+address byte, `W 3C` data
written, `R` read request, `P` stop, `ACK n`). The lanes show the *messages*
simavr passes, one per byte/condition, not clock/data waveforms, and a TWI
address label is the raw address byte as the model passes it. They are in the
VCD as `spi0_out`/`spi0_in` (8-bit) and `twi0_out`/`twi0_in` (32-bit raw
message word) and in the session. The marker lanes now share one drawing path
(labels that don't fit collapse to ticks). A device without the peripheral
rejects the name. Tests inject wire events (decoding, lanes, VCD, session,
paint); no firmware-driven SPI/TWI transfer is run end to end.

**USART lanes (slice 17, 2026-10-19):** `USART0`-`USART3` in the pin box add `tx` +
`rx` lanes (`usartN_tx`/`usartN_rx` in the VCD, 8-bit) from the USART's wire IRQs;
USART0 adds only `rx` because its TX is the built-in "UART0 TX" lane. RX is every
byte that reaches the USART's input (typed into the UART dock, sent with
`uart_send`, or driven by an attached part), at the cycle it arrived, not the
cycle the firmware reads it. Labels are the character, or hex if unprintable. An
end-to-end test (real firmware with RXEN set, a byte fed in) checks the RX lane;
the TX lanes of USART1-3 are covered by injected events only.

**Vector names (slice 18, 2026-10-19):** the Interrupts pane shows `N NAME` and the
IRQ lane labels with the name (`TCA0_OVF`), taken from the `<NAME>_vect_num`
defines of the device's avr-libc header (`simavr/cores/avr/` in this checkout,
else `$AVR_INCLUDE` / the avr-gcc install; `simavr_py.vectors`). Vector 0 is
`RESET`. No header found (or a non-tiny/mega device) falls back to the bare
number; the VCD still carries the number.

**CPUINT state (slice 19, 2026-10-19):** the Interrupts pane shows `executing: ...`
from CPUINT.STATUS (data 0x0111: NMIEX bit 7, LVL1EX bit 1, LVL0EX bit 0, checked
against DS40002205A 13.5.2), innermost level first (`NMI > LVL1 > LVL0`, or `none`).
It is read on each refresh while stopped and left as-is while running.

**Pending interrupts (slice 20, 2026-10-19):** `avr_host_pending_interrupts` returns the
vectors the engine has queued (raised *and* enabled, not yet taken), ascending, only while
paused. Python: `Simulator.pending_interrupts()`; the Interrupts pane shows
`pending: 12 TCA0_OVF, ...` above the `flags set:` line (which is the raw INTFLAGS view,
including flags whose enable bit is clear). A pending vector still needs SREG.I and a
sufficient CPUINT level to be taken, so the pane does not claim it is next. Not done:
per-vector level in the pending list, and dispatch order.

1. Breakpoints:
   * PC breakpoints
   * data watchpoints and peripheral-register change triggers
2. Add disassembly and source correlation from ELF/DWARF where available.
3. Show interrupt activity:
   * pending/active sources
   * vector taken
   * CPUINT state for modern AVR
4. Add timeline views for selected pins (done), timers, and bus traffic without forcing
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
