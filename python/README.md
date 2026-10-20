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

![simavr GUI running an ATtiny3217 that drives an SSD1306 OLED over TWI0](../doc/img/gui-attiny3217-oled.png)

*The GUI running `examples/demo_3217_oled.c` on an ATtiny3217 with the `boards/oled_3217.json`
board: the SSD1306 on TWI0 (PB0/PB1) shows a title and a live counter, LED0 on PA0 toggles with
each update, and the dock tabs show registers, ports, CPU state and disassembly.*

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
or momentary), a rotary encoder, an HD44780 LCD and a 128x64 SSD1306 OLED (I2C or
4-wire SPI). Define your own in JSON/TOML (see `boards/demo_4809.json` and
`simavr_py.boards`). An OLED is `{"type": "oled", "bus": "twi", "addr": 60}` or
`{"type": "oled", "bus": "spi", "dc": "PC1", "cs": "PC0", "res": "PC2"}`; the bus pins are
the firmware's, `cs`/`dc`/`res` are the GPIOs the panel listens to.

Shipped boards in `boards/`:

| File | MCU | Wired parts |
|---|---|---|
| `demo_4809.json` | atmega4809 | 3 LEDs, 2 buttons, encoder, 16x2 LCD (matches `examples/demo_4809.c`) |
| `oled_4809.json` | atmega4809 | SSD1306 on TWI0 at 0x3C (run `examples/board_atmega4809_oled/atmega4809_oled_i2c.c`) |
| `demo_4808.json` | atmega4808 | same layout as the 4809 demo |
| `demo_3217.json` | attiny3217 | 3 LEDs, 2 buttons, encoder, 16x2 LCD |
| `demo_1614.json` | attiny1614 | 3 LEDs, 2 buttons, encoder |
| `demo_212.json` | attiny212 | 2 LEDs, 2 buttons |
| `oled_3217.json` | attiny3217 | LED0 + SSD1306 on TWI0 at 0x3C (matches `examples/demo_3217_oled.c`, pictured above) |
| `demo_13a.json` | attiny13a | 2 LEDs, 1 button (matches `examples/demo_13a.c`: Timer0 blink, pull-up button, ADC) |
| `curiosity_nano_4809.json` | atmega4809 | ATmega4809 Curiosity Nano: LED0 on PF5 (active low), SW0 on PF6 |
| `curiosity_nano_3217.json` | attiny3217 | ATtiny3217 Curiosity Nano: LED0 on PA3 (active low), SW0 on PB7 (matches `examples/demo_3217_nano.c`) |

The `demo_*` layouts are generic; only `demo_4809`, `demo_13a` and `oled_3217` have matching firmware (the Nano boards: `curiosity_nano_3217` has `demo_3217_nano`). The
Curiosity Nano pins come from Microchip's board user guides. Neither Nano has an
external pull-up on SW0, so the firmware must enable the internal one.

**Classic AVRs (ATtiny13A and the other pre-modern cores):** the register, port, timer, analog,
serial, clock, system and interrupt docks also work there, read from the flat `_SFR_IO8` form of the
device header (`simavr_py.classic`): registers keep their header names (`PORTB`, `TCCR0B`),
consecutive bit names that share a stem form one field (`CS00..CS02` -> `CS0=n`), and the Ports dock
lists only the pins the header defines (ATtiny13A: PB0-PB5) with the pull-up derived from DDxn,
PORTxn and PUD. On the ATtiny13A the Timers dock adds derived rows from DS40002307A Tables
11-2..11-9 (clock source and tick rate, mode and TOP, what OC0A/OC0B do, counter cycle) and the
Clock dock decodes CLKPS; CLKPR writes really rescale the simulated clock (CLKPCE sequence, CKDIV8
reset value 3, restored on reset). The Interrupts dock lists TIFRn/GIFR flags with their masks and
the flags that share a register with their enable (ADIF, ACI, WDTIF) as enabled/masked. There are
no CPUINT levels, PORTMUX or Events on these parts, and those docks say so. The core name you
asked for (`attiny13a`, not the shared `attiny13` descriptor) is what `Simulator.mmcu` reports.

**Docks** (drag to rearrange; related ones share tabs, the default is right column =
CPU plus tabs, bottom = Timeline plus Memory/UART tabs):
- **CPU**: state, cycle, time, PC/SP/SREG and r0-r31 (live while running, from a consistent snapshot).
- **Disassembly**: instructions around PC with symbols and source lines; the PC marker (and the Source pane's current line) follows the PC while running; double-click a row to toggle a breakpoint, right-click for **Run to here**.
- **Source**: the C source with the current line (needs `-g`); double-click toggles a breakpoint, right-click offers **Run to line**.
- **Watch**: `[label=]hexaddr|REGISTER[:width]`, e.g. `porta_out=0x0405` or `TCA0.SINGLE.CNT`, live data-space values.
- **Breakpoints**: hex address or symbol (`main`, `main+0x4`); `0xadd` forces an address over a symbol.
- **Data Breakpoints**: `hexaddr[:len] [r|w|rw|c|p]` (read/write/either/change/any peripheral-register change), e.g. `0x0400`, `3f:2 rw`.
- **Interrupts**: per-vector count, level and last cycle, vector names, and which CPUINT level is executing.
- **Ports**: per pin, direction, OUT latch, input level, invert, pull-up and sense config from the PORT registers.
- **PORTMUX**: each routing field (USART0, TCB2, ...) with its register value, the option selected and the pins it names (tinyAVR 0/1 headers say only "Default/Alternate pins"). Bold = not the reset routing. Routing is live for USART0 TXD and TCA0 WO0-2 on the ATtiny3216/3217 (the signal appears on the selected pin); other fields and devices are stored and shown but not yet routed.
- **Timers**: TCA (single and split mode), TCB and TCD register state, one row per field: enable, clock, mode, CNT/PER/compares, output enables, interrupt enables and flags. Names and masks come from the device header (TCD WGMODE has no enum there, so it shows the header's name only where one exists). A register view only; the Timeline draws the actual outputs.
- **Events**: Event System routing: each channel's generator and, per user (CCL LUT inputs, ADC, TCA/TCB, USART, EVOUT), the channel it listens to, e.g. `CHANNEL0  (PORT0_PIN2)`. Handles both layouts (`CHANNELn`/`USER*` on megaAVR-0 and tinyAVR 2-series, `ASYNCCH`/`SYNCCH` + `ASYNCUSER`/`SYNCUSER` on tinyAVR 0/1). Names come from the device header; a register view only, not events in flight.
- **Analog**: ADC, VREF, AC and DAC register state, one row per register with its value and every field the device header defines for it (prescaler, reference, input mux, hysteresis, interrupt enables and flags ...). Derived rows come from data-sheet arithmetic: VREF options show their level (`2V5 (2.5 V)`), each ADC shows its reference and last result in volts (VDD references use the **VDD** box, since the simulator has no supply register; an external VREFA has no known voltage so no result), and an AC with DACREF shows the divider output. Mux options name the pad (`MUXPOS=AIN3 (PD3)`) from `analog_pins.json`, generated from Microchip's ATDF files by `python tools/gen_analog_pins.py <atdf dirs>`; ATtiny3214 has no ATDF, so it shows no pad names. Layouts follow the device's own header (tinyAVR 2-series ADC with PGA/MUXNEG, tinyAVR 1-series four-register VREF). A register view only.
- **Serial**: USART, SPI and TWI register state, decoded like Analog (value plus every field the device header defines; reads have no side effects, so RXDATA is not consumed). Each USART gets a **Baud rate** row from BAUD by DS40002173C Table 23-1 (`64 x fCLK_PER / (S x BAUD)`, S = 16 normal, 8 CLK2X; synchronous uses BAUD[15:6]), assuming CLK_PER equals the simulated CPU clock (no CLKCTRL prescaler is read). A register view only.
- **Clock**: CLKCTRL register state decoded like Analog (value plus every header-defined field), with derived rows for the main clock source (CLKSEL), the prescaler (PEN/PDIV, DS40002205A 10.5.1-10.5.2) and the resulting CLK_PER (the simulated CPU clock) and CLK_MAIN (CLK_PER x division). A register view.
- **System**: RSTCTRL, SLPCTRL, BOD, WDT, CPUINT, CRCSCAN, RTC/PIT, CCL, NVMCTRL and SYSCFG register state decoded like Analog (value plus every header-defined field), for the blocks the device has. A register view, no derived values.
- **Timeline**: type a pin (`PA0`), timer output (`TCA0.WO1`) or bus (`SPI0`, `TWI0`, `USART1`) + Enter; wheel zooms, drag pans, click/right-click set cursors A/B, `Export VCD…` writes a GTKWave file. Also shows UART0 TX and interrupt vectors.
- **Memory**: data / flash / eeprom hex view at an address.
- **UART**: console for USART0; type a line + Enter to send it to the firmware.

**Help** (`Help` menu, F1): a searchable in-app manual covering every pane, the toolbar, boards, debugging, the Timeline and sessions (`simavr_py/gui/help.py`).

**Sessions** (`Session` menu): save and load breakpoints, watches, board, pins, layout,
firmware and MCU. The window layout, MCU, clock and last firmware folder are also
remembered automatically between launches (`~/.config/simavr/simavr-gui.conf`);
explicit command-line flags win.

Limits and per-feature details: [`doc/python_gui_todo.md`](../doc/python_gui_todo.md).
