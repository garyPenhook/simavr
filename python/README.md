# simavr-py

Python bindings for simavr's host API ([`simavr/sim/sim_host.h`](../simavr/sim/sim_host.h)),
the embedding boundary for the planned Python GUI
([`doc/python_gui_todo.md`](../doc/python_gui_todo.md)). Draft: follows the
C API, which is not ABI-stable yet.

Requires the project's Python 3.15 environment (`.python-version`, `.venv`)
and a built `libsimavr.so`. It uses cffi in ABI mode, so nothing is compiled
at install time.

```sh
make -C simavr                                  # builds simavr/obj-*/libsimavr.so
uv pip install --python .venv/bin/python -e python
.venv/bin/python -m pytest python               # needs pytest (dev group)
```

The library is found in the checkout's `simavr/obj-*/` first, then on the
system; set `SIMAVR_LIB=/path/to/libsimavr.so` to override.

```python
from simavr_py import Simulator, Space, PinEvent

with Simulator("atmega4809") as sim:
    sim.load_elf("blink.elf")
    led = sim.subscribe_pin("PA0", coalesce_cycles=1000)
    sim.start()                     # library-owned simulation thread
    sim.resume()
    for ev in sim.wait_events(timeout=0.5):
        if isinstance(ev, PinEvent):
            print(ev.cycle, ev.level)
    sim.pause()
    sim.wait_stopped()
    print(sim.cpu.pc, sim.read(Space.DATA, 0x0400, 1))
```

Threading: each call drops the GIL and the C side serialises access with its
own lock, so one `Simulator` can be used from several threads, including on
the free-threaded (no-GIL) 3.15 build. Don't call `close()` while another
thread is still using the instance. `fileno()` is readable whenever events
are pending, so it can drive `select` or Qt's `QSocketNotifier` directly.

Calls that the C API doesn't implement yet (register table, symbols, analog
input, log text, VCD) aren't wrapped. Watchpoints and interrupt events are
(`add_watchpoint`, `subscribe_interrupts`).

## GUI

A desktop front-end (PySide6/Qt) for loading firmware, running it, poking pins and
debugging, with no VCD/GTKWave step needed for everyday work.

### Starting it

From the repo root:

```sh
./simavr-gui                              # empty window; pick MCU, then Open…
./simavr-gui firmware.elf                 # load straight away (MCU comes from the ELF)
./simavr-gui -m attiny3217 -f 3333333 firmware.hex
./simavr-gui --board python/boards/demo_4809.json python/examples/demo_4809.elf
```

The launcher builds `libsimavr` and creates `.venv` with PySide6 on first use, and
works from any directory (symlink it onto your `PATH` if you like). Options:

| Option | Meaning |
|---|---|
| `firmware` | ELF (`.elf`/`.axf`) or HEX image |
| `-m, --mmcu` | MCU name; needed for HEX or an ELF without an `.mmcu` section |
| `-f, --freq` | clock in Hz (0 = from the ELF / device default) |
| `--board FILE` | virtual board (`.json`/`.toml`) |
| `--session FILE` | restore a saved session |
| `--speed N` | 1.0 = real time (default), 0 = unthrottled |

Manual install instead of the launcher:
`uv pip install --python .venv/bin/python -e 'python[gui]'` then
`.venv/bin/python -m simavr_py.gui ...`.

Quick try: **Open…** `python/examples/demo_4809.elf` (it embeds its MCU, ATmega4809),
then **Board…** `python/boards/demo_4809.json`, then **Run**. LED0/LED1 follow PA0/PA1,
the LCD shows a counter, and the UART dock prints `hello N ticks`.
Build your own firmware with a modern `avr-gcc`, e.g.
`avr-gcc -mmcu=atmega4809 -Os -g -o fw.elf fw.c` (`-g` gives source view and line stepping).
Pick `.axf`/`.elf` files that are *AVR* binaries; a host program such as a board's
`oled.elf` under `obj-*/` will fail with "cannot read ELF".

### Using it

**Toolbar:** `Open…` load firmware · MCU box (every compiled core; `(from ELF)` uses
the ELF's own; changing it reloads the loaded firmware) · clock box (Hz, `auto` = default) ·
`Board…` · `Run` · `Run for` (with its ms box: run that much simulated time, then pause) · `Pause` · `Step` (one instruction) · `Next line` (step over a source
line or call) · `Step out` (run to the current function's return) · `Reset`.
Run controls are disabled while they can't apply (e.g. Step while running).

**Central pin panel:** one cell per pin showing level and direction (`OUT 1`, `Z 0`
= input/high-Z). Tick **drive** to drive an input pin from outside, **H** for the level.

**Board dock (optional):** virtual parts wired to pins: LEDs, push buttons (latching
or momentary), a rotary encoder and an HD44780 LCD. Define your own in JSON/TOML
(see `boards/demo_4809.json` and `simavr_py.boards`).

Shipped boards in `boards/`:

| File | MCU | Wired parts |
|---|---|---|
| `demo_4809.json` | atmega4809 | 3 LEDs, 2 buttons, encoder, 16x2 LCD (matches `examples/demo_4809.c`) |
| `demo_4808.json` | atmega4808 | same layout as the 4809 demo |
| `demo_3217.json` | attiny3217 | 3 LEDs, 2 buttons, encoder, 16x2 LCD |
| `demo_1614.json` | attiny1614 | 3 LEDs, 2 buttons, encoder |
| `demo_212.json` | attiny212 | 2 LEDs, 2 buttons |
| `curiosity_nano_4809.json` | atmega4809 | ATmega4809 Curiosity Nano: LED0 on PF5 (active low), SW0 on PF6 |
| `curiosity_nano_3217.json` | attiny3217 | ATtiny3217 Curiosity Nano: LED0 on PA3 (active low), SW0 on PB7 (matches `examples/demo_3217_nano.c`) |

The `demo_*` layouts are generic; only `demo_4809` has matching firmware (the Nano boards: `curiosity_nano_3217` has `demo_3217_nano`). The
Curiosity Nano pins come from Microchip's board user guides. Neither Nano has an
external pull-up on SW0, so the firmware must enable the internal one.

**Docks** (drag to rearrange; related ones share tabs, the default is right column =
CPU plus tabs, bottom = Timeline plus Memory/UART tabs):
- **CPU**: state, cycle, time, PC/SP/SREG and r0-r31 (refreshed while paused; shown stale while running).
- **Disassembly**: instructions around PC with symbols and source lines; double-click a row to toggle a breakpoint, right-click for **Run to here**.
- **Source**: the C source with the current line (needs `-g`); double-click toggles a breakpoint, right-click offers **Run to line**.
- **Watch**: `[label=]hexaddr[:width]`, e.g. `porta_out=0x0405`, live data-space values.
- **Breakpoints**: hex address or symbol (`main`, `main+0x4`); `0xadd` forces an address over a symbol.
- **Data Breakpoints**: `hexaddr[:len] [r|w|rw|c|p]` (read/write/either/change/any peripheral-register change), e.g. `0x0400`, `3f:2 rw`.
- **Interrupts**: per-vector count, level and last cycle, vector names, and which CPUINT level is executing.
- **Timeline**: type a pin (`PA0`), timer output (`TCA0.WO1`) or bus (`SPI0`, `TWI0`, `USART1`) + Enter; wheel zooms, drag pans, click/right-click set cursors A/B, `Export VCD…` writes a GTKWave file. Also shows UART0 TX and interrupt vectors.
- **Memory**: data / flash / eeprom hex view at an address.
- **UART**: console for USART0; type a line + Enter to send it to the firmware.

**Sessions** (`Session` menu): save and load breakpoints, watches, board, pins, layout,
firmware and MCU. The window layout, MCU, clock and last firmware folder are also
remembered automatically between launches (`~/.config/simavr/simavr-gui.conf`);
explicit command-line flags win.

Limits and per-feature details: [`doc/python_gui_todo.md`](../doc/python_gui_todo.md).
