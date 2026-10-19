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

Calls that the C API doesn't implement yet (register table, symbols,
watchpoints, analog input, interrupt events, VCD) aren't wrapped.

## GUI

```sh
uv pip install --python .venv/bin/python -e 'python[gui]'
.venv/bin/python -m simavr_py.gui -f 3333333 blink.elf   # --speed 0 = unthrottled
```

Boards: `--board python/boards/demo_4809.json` shows virtual LEDs, buttons and
a rotary encoder wired to pins by a JSON/TOML file (see `simavr_py.boards`).
