# Code review — modern-AVR engine & peripherals (2026-09-24)

Scope: the fork's modern-AVR (AVRxt) work. That covers the CPUINT dispatch path in
`sim_interrupts.c`, the `arch` hooks in `sim_core.c`/`sim_avr.c`, and the modern
peripheral models under `simavr/sim/` (TCA, TCB, TCD, RTC, USART, PORT, CLKCTRL,
CPUINT, WDT, NVMCTRL, CCL). The frozen classic path was not reviewed. SPI, TWI,
ADC, AC, DAC, EVSYS, BOD and CRCSCAN only got a skim.

Baseline at review time: `make -C simavr` builds with no warnings, even with
`-Wall -Wextra -Wshadow`. `make -C tests run_tests` gives **34/34** pass, and the
`test_avrxt_engine` self-check passes. None of the bugs below is caught by the
current tests.

Confidence tags:
- **[confirmed]**: reproduced with a small host program against `libsimavr`
  (repro sketch included).
- **[code]**: follows directly from reading the code; not run.
- **[DS]**: the hardware behaviour was checked against the Microchip datasheet
  (DS40002205A, ATtiny3216/17) through the Microchip documentation MCP.
- **[verify]**: a hardware-semantics point that still needs a datasheet check
  before anyone changes it.

Items already listed in `doc/modern_support_todo.md` are left out, except where
this review found that the TODO understates the problem.

---

## Summary

| # | Sev | Area | Finding | Tag |
|---|-----|------|---------|-----|
| 1 | **High** | CPUINT engine | **FIXED.** Stale pending-FIFO entries pile up. After ~63 flag clears, new interrupts are silently lost and stay stuck forever | confirmed |
| 2 | **High** | CPUINT / reset | `CPUINT.STATUS` survives reset. A WDT/SW reset taken inside an ISR blocks all LVL0 interrupts permanently | confirmed |
| 3 | **High** | PORT | `PINnCTRL.INVEN` makes `PORTx.IN` flip on every read | confirmed + DS |
| 4 | **High** | TCB | PWM8 CAPT events are dropped when the cycle timer fires late. A `while(1);` loop loses 50 % of them | confirmed |
| 5 | Med | WDT | Window mode: timeout is PERIOD from arm. It should be WINDOW + PERIOD, with the closed window starting at the first WDR | DS |
| 6 | Med | RTC | Cycles-per-tick is truncated to an integer: 0.7 % fast at the default 3.33 MHz | code |
| 7 | Med | CLKCTRL / TCD / BOD | Fuse-derived state (OSCCFG FREQSEL, BODCFG) is snapshotted before the ELF's `.fuse` section is loaded | code |
| 8 | Med | CLKCTRL | Every reset forces `avr->frequency` back to OSC20M/6, which overrides `-f` / `.mmcu` frequency after a WDT/SW reset | code |
| 9 | Med | TCD | Any CMPxSET/CLR, CTRLB or FAULTCTRL write restarts the counter from 0. Per-period duty updates can starve OVF | code |
| 10 | Med | USART | Never raises `UART_IRQ_OUT_XON`, so the stock `uart_pty`/`uart_udp` parts never deliver input to modern firmware. IRQ names use the wrong direction prefix | code |
| 11 | Med | USART | TX is not rate-limited: DREIF is set again immediately, so unlimited bytes/s go out. Only TXC is delayed | code |
| 12 | Med | TCB | Writes to `CNT` while running are ignored (no write handler) | code |
| 13 | Med | TCA | `PERBUF`/`CMPnBUF` (buffered, glitch-free updates) are plain RAM, so writes never reach PER/CMPn | code |
| 14 | Med | NVMCTRL | CPU can run FUSEWRITE (UPDI-only on hardware) and it takes effect immediately. CHIPERASE ignores `SYSCFG0.EEESAVE` | DS |
| 15 | Med | Interrupts (all modern peripherals) | Interrupts are edge-modelled: enabling INTCTRL after the flag is set, or returning from an ISR without clearing the flag, does not (re)fire. Only RTC and USART handle the enable-after-flag case | code / verify |
| 16 | Low | CPUINT engine | `CLI` zeroes `interrupt_state` even on CPUINT cores, which can hide a pending NMI until the next raise/RETI | code |
| 17 | Low | CCP | `arch.ccp_window` is not cleared on reset | code |
| 18 | Low | PORT | `ISC = INPUT_DISABLE` still updates `PORTx.IN` | DS |
| 19 | Low | Sleep | "Sleeping with I=0 → quit" also applies to modern cores, where an NMI can still wake the core | code |
| 20 | Low | CCL | Filter/edge/sequencer LUTs schedule a timer **every CPU cycle** (big slowdown) | code |
| 21 | Low | Docs | `CLAUDE.md` says 31 test groups; the suite now runs 34 | — |

---

## Details

### 1. Stale entries in the CPUINT pending FIFO → lost interrupts (High) [confirmed] — **FIXED**: `avr_clear_interrupt()` now dequeues the vector on CPUINT cores; regression check in `test_avrxt_engine.c`.

`sim_interrupts.c:356` (`avr_service_interrupts_modern`):

```c
if (!v->pending || !avr_regbit_get(avr, v->enable))
    continue;
```

Entries whose vector is no longer pending, or is disabled, are skipped but
**never dequeued**. The classic path drops them. Every flag clear that goes
through `avr_clear_interrupt()` while the vector is queued but not yet serviced
leaves one dead entry behind. Examples are a W1C to INTFLAGS, reading RXDATA, or
reading TCB CCMP, done in a `cli()` section or with a higher level running.
Re-raising the vector queues a new entry. The FIFO holds 64 entries
(`sim_interrupts.h:58`), and `avr_int_pending_write()` drops silently when full.
Once it is full, the next *different* vector gets `pending = 1` but is never
queued. Because `pending` is already 1, later raises return early, so that
vector is dead until reset. A second problem: `avr_has_pending_interrupts()`
stays true, so `interrupt_state` stays 1 and the run loop (`sim_core.c:1702`)
never takes its fast path.

Repro (attiny3217): enable vector A, set I=0, run `raise(A); clear(A)` 70 times,
then enable and raise vector B, set I=1 and service. Result: FIFO = 63 entries,
B stays `pending = 1`, and it is never dispatched (`pc` unchanged, STATUS 0).

Fix: in the modern loop, remove entries with `!v->pending` (and optionally
disabled ones) from the FIFO, or have `avr_clear_interrupt()` remove the vector's
entry. A cheaper option is a per-vector "queued" bit so a vector can be in the
FIFO at most once.

### 2. `CPUINT.STATUS` is not cleared on reset (High) [confirmed]

`table->cpuint_status` is zeroed only by the `memset` in `avr_interrupt_init()`.
Neither `avr_interrupt_reset()` (`sim_interrupts.c:48`) nor `avr_cpuint_reset()`
clears it. A reset taken while an ISR runs leaves LVL0EX/LVL1EX/NMIEX set, and
the new program can never take an interrupt at that level or below. The usual
ways this happens are a watchdog firing because an ISR hung (the case watchdogs
exist for), or `RSTCTRL.SWRR` issued from a command-handling ISR.

Repro: dispatch one LVL0 interrupt (STATUS=0x01), call `avr_reset()`, and STATUS
is still 0x01. The next raised interrupt is not dispatched.

Fix: `table->cpuint_status = 0;` in `avr_interrupt_reset()`.

### 3. `PINnCTRL.INVEN` makes `PORTx.IN` alternate (High) [confirmed, DS]

`avr_port_modern_in_read()` (`avr_port_modern.c:191`) computes
`v = (data[IN] & ~dir) | (out & dir); v ^= inv;` and then **stores `v` back
into `data[IN]`**. The next read inverts the stored, already inverted value
again. With PA0 an input and INVEN=1, four consecutive reads return
`0x01, 0x00, 0x01, 0x00`. DS40002205A §16.3.2.2 / §16.5.11: INVEN inverts input
and output values, so it should read a steady inverted value. Also,
`avr_port_modern_irq_notify()` stores the *uninverted* level in `data[IN]` and
does edge detection on it. The datasheet says toggling INVEN itself produces an
edge that interrupts and events see, and the model doesn't produce that edge.

Fix: keep the raw pin level in a private field (not `data[IN]`), apply INVEN
only on the read path, and run edge detection on the inverted value.

### 4. TCB PWM8 misses CAPT when the timer callback runs late (High) [confirmed]

`avr_tcb.c:285`: the PWM8 tick decides it has reached the wrap by testing
`tcb_current_cnt(p) == 0`. That value is computed from `avr->cycle`, but cycle
timers fire at the end of the instruction that crosses `when`, which can be
1–4 cycles late. When it is late, CNT reads 1..3, the wrap is missed, CAPT is not
raised and the counter is not re-anchored. Every TCB tick path
(`tcb_set_running(p, 1, 0)` at lines 262/276/287, which sets
`start_cycle = avr->cycle`) also anchors the phase to `avr->cycle` instead of
`when`, so CNT readback drifts by the lateness each period.

Repro: TCB0 PWM8, CCMPL=10, CCMPH=5, run for 10 000 cycles.
- A field of NOPs (1-cycle instructions): 909 CAPT pulses (expected 909).
- A field of `rjmp .-2`, the `while(1);` idle loop (2 cycles): **454** CAPT
  pulses.

Fix: in tick callbacks, anchor with `when` (as TCA/RTC/TCD already do), and
decide wrap vs. CCMPH match from the scheduled target, not from the live
`avr->cycle`-derived count.

### 5. WDT window mode timing (Medium) [DS]

DS40002205A §19.3.3.2: in window mode the timeout is the closed window (WINDOW)
followed by the open window (PERIOD). "The total duration of the time-out period
is the sum of the closed window and the open window". Also, "the first closed
period is activated after the first WDR". `wdt_arm()` (`avr_wdt.c:100`) arms the
reset at `timeout_cycles` (PERIOD only) and applies the closed window from the
CTRLA write. So with WINDOW ≥ PERIOD the device resets before the open window
even begins. Firmware that services the dog correctly in the open window gets
reset.

### 6. RTC tick period truncated to integer cycles (Medium) [code]

`rtc_cnt_cpu_per_tick()` (`avr_rtc.c:120`) returns
`floor(f_CPU · prescale / f_RTC)`. At the default 3.333 MHz with 32.768 kHz and
DIV1 that is 101 instead of 101.72 cycles, so the RTC runs **0.71 % fast**
(~26 s/hour). A 1-second `PER=32767` overflow happens after 3 309 568 cycles
instead of 3 333 333. PIT has a smaller version of the same error. Fix: compute
event times as `start + target · f_CPU · div / f_RTC` in 64-bit, instead of
multiplying a truncated per-tick value.

### 7. Fuse-derived state is read before the ELF's fuses are loaded (Medium) [code]

`avr_clkctrl_init()` (`avr_clkctrl.c:202`) and `avr_tcd_init()`
(`avr_tcd.c:451`) resolve `freq_osc20m` from `FUSE.OSCCFG` at **init**.
`avr_bod_reset()` reads `BODCFG` at reset. `run_avr` (and most embedders) call
`avr_init()` (which resets) *before* `avr_load_firmware()` fills `avr->fuse[]`,
and there is no reset after loading. So an ELF built with
`FUSES = { .OSCCFG = FREQSEL_16MHZ_gc, ... }` still runs from a 20 MHz OSC20M,
making all timing 25 % off, and BOD starts with the pre-load fuse. Fix: read the
fuse at reset, or on first use, and have `avr_load_firmware()` (or the modern
cores) re-apply fuse-derived state after the fuses are loaded.

### 8. CLKCTRL reset overrides the configured frequency (Medium) [code]

`avr_clkctrl_reset()` runs `avr_clkctrl_recompute()`, which sets
`avr->frequency = OSC20M / 6`. At startup that is harmless because
`avr_load_firmware()` then applies `-f` / the `.mmcu` frequency. On **every
later reset** (WDT, SWRST, BOD) the user's frequency is lost and the simulation
silently switches clock. The register state (`MCLKCTRLB = PEN|6X`) and
`avr->frequency` also disagree whenever `-f` is used. The code should settle on
one source of truth, e.g. treat `-f` as the OSC20M base, or skip recompute on
reset when the user pinned the frequency.

### 9. TCD restarts on any compare/config write (Medium) [code]

`avr_tcd_cfg_write()` (`avr_tcd.c:382`) calls `avr_tcd_reschedule()`, which sets
`start_count = 0; phase = 0`. Each byte written to CMPASET/CMPACLR/CMPBSET/CMPBCLR
restarts the waveform from BOTTOM. A 16-bit update is two restarts. Firmware
that updates the duty every period (motor control, SMPS) will keep the counter
from ever reaching TOP, and OVF never fires. The real TCD double-buffers the
compare registers and applies them on a SYNC/SYNCEOC command (CTRLE), which is
not modelled. Also, the TODO's TCD row lists only clock-source gaps, so this
should be added there. [verify: CTRLE command semantics and which TCD registers
are enable-protected.]

### 10. Modern USART never raises XON; IRQ names are the wrong direction (Medium) [code]

The classic UART raises `UART_IRQ_OUT_XON` when its RX FIFO drains
(`avr_uart.c:159/208/420`). `examples/parts/uart_pty.c` feeds bytes only while
`p->xon` is set, and the only thing that sets it is the XON hook. The modern
USART never raises XON/XOFF, so `uart_pty` (and `uart_udp`) never send host
input to modern firmware. The names `"<usart_xon"`/`"<usart_xoff"`
(`avr_usart_modern.c:225`) also mark them as *inputs*. The classic names are
`">xon"`/`">xoff"`. Also, a full 256-byte RX FIFO drops bytes silently, with no
BUFOVF flag.

### 11. USART TX has no baud throttling (Medium) [code]

`avr_usart_modern_txdata_write()` emits the byte immediately and sets DREIF again
at once (`avr_usart_modern.c:134`). Only TXC is delayed by the frame time, so a
DRE-driven or polling transmitter sends any amount of data in near-zero simulated
time. Timing-dependent firmware (timeouts, protocol pacing) behaves differently
from hardware. The frame length is also fixed at 10 bits, ignoring
CHSIZE/PMODE/SBMODE. The TODO says async TX timing "works", which overstates
this.

### 12. TCB: CNT writes while running are ignored (Medium) [code]

No write handler is registered on `r_cnt`/`r_cnt+1` (`avr_tcb_init()`). Writing
`TCB0.CNT = 0` while enabled only changes `data[]`. The next read recomputes CNT
from `start_cycle`/`start_count` and overwrites it, and the scheduled CAPT is not
moved. TCA and RTC both handle this case (`avr_tca_cnt_write`,
`avr_rtc_cnt_write`).

### 13. TCA buffered registers are inert (Medium) [code]

`PERBUF` and `CMPnBUF` (base+0x36..0x3D) have no write handler and are never
transferred to PER/CMPn at UPDATE. Firmware that uses the buffered registers for
glitch-free PWM updates, a common pattern, never changes the duty. The related
`CTRLFSET/CLR` BV flags aren't modelled either. This isn't in the TODO. The TODO
does already list CTRLE commands and split mode.

### 14. NVMCTRL fuse/chip-erase semantics (Medium) [DS]

DS40002205A §9.3.2.4.7: Write Fuse "can only be used by the UPDI; the CPU cannot
start this command", and "a Reset is required for the updated value to take
effect". The model (`avr_nvmctrl.c:236`) lets the CPU run FUSEWRITE and updates
`avr->fuse[]` immediately. IVSEL/BOOTEND and CRCSCAN read fuses live, so the
change takes effect at once. §9.3.2.4.5: Chip Erase leaves EEPROM unaltered when
`FUSE.SYSCFG0.EEESAVE` is set. The model (`avr_nvmctrl.c:225`) always erases it.
[verify: whether CHER is CPU-startable at all on these parts.]

### 15. Interrupts are edge-modelled, not level (flag & enable) (Medium) [code / verify]

`avr_raise_interrupt()` queues a vector only if it is enabled at raise time, and
the dispatcher clears `pending` on entry. The sticky flag stays set, but nothing
re-raises it. Two cases differ from hardware:
- Enabling an interrupt (INTCTRL) after its flag is already set does not fire.
  RTC and USART special-case this in their INTCTRL/CTRLA writes. TCA, TCB, TCD,
  PORT, AC, ADC, NVMCTRL, SPI and TWI do not.
- An ISR that returns without clearing its flag is not re-entered. On hardware
  that is the classic "forgot to clear the flag" hang. A simulator that hides it
  is less useful for debugging.

A generic fix is in the engine: on RETI and on any write to a vector's
`enable.reg`, re-queue vectors whose `raised` and `enable` bits are both set.
[verify: level semantics per peripheral in the datasheet interrupt chapters.]

### 16. `CLI` clears `interrupt_state` on CPUINT cores (Low) [code]

`avr_sreg_set(S_I, 0)` (`sim_core.h:131`) sets `interrupt_state = 0`
unconditionally. On CPUINT cores a queued NMI must still be serviced, but it is
no longer re-evaluated until another raise or a RETI. In practice this is narrow,
because service normally runs right after each instruction. It is reachable
inside a CCP window, where service returns early while `interrupt_state` stays 1.
Keep `interrupt_state` when `AVR_ARCH_F_CPUINT` is set.

### 17. CCP window survives reset (Low) [code]

`avr_reset()` doesn't clear `avr->arch.ccp_window`. A reset issued right after a
CCP unlock (e.g. `CCP=IOREG; RSTCTRL.SWRR=1`) leaves the first few instructions
of the new program with protected writes unlocked.

### 18. `ISC = INPUT_DISABLE` not honoured (Low) [DS]

DS40002205A §16.5.11 note 1: when the digital input buffer is disabled, `IN` bit
n is not updated. `avr_port_modern_irq_notify()` updates `data[IN]` whatever ISC
is set to.

### 19. Sleep with I=0 ends the simulation on modern cores (Low) [code]

`avr_callback_run_raw/gdb` (`sim_avr.c:468/526`) stop the simulation
("sleeping with interrupts off") when the core sleeps with I=0. On CPUINT cores
an NMI (CRCSCAN) can still wake it, and some firmware uses I=0 sleep with
wake-up by event. Gate the check on `!(arch.flags & AVR_ARCH_F_CPUINT)` or on no
NMI-capable vector being enabled.

### 20. CCL per-cycle timer (Low, performance) [code]

`avr_ccl_tick()` returns `avr->cycle + 1` (`avr_ccl.c:377`) whenever a LUT uses
the filter, edge detector or sequencer. That is one timer callback per simulated
cycle for as long as it stays enabled. This makes simulations with a CCL filter
or sequencer much slower. It would be better to schedule on input changes and
only for the 2–4 cycle filter/edge windows. The TODO mentions "filter/edge
callback cost" under TCB, but it is a CCL issue.

### 21. Docs drift (Low)

`CLAUDE.md` ("31 test groups", "~380 assertions") no longer matches: the suite
now reports 34 runs. The "Modern-processor support status" section says modern
processors are "fully supported". Items 1–4 above are correctness bugs that
typical firmware hits, so that claim should be softened until they are fixed.

---

## Unverified follow-ups (need a datasheet pass before acting)

- Whether `FUSE.WDTCFG` should be loaded into `WDT.CTRLA` at reset (always-on
  watchdog). `avr_wdt.c` never reads it. The MCP search did not return the
  tinyAVR-1 fuse text.
- Whether a TCB/TCA CCMP/PER write below the current count runs to MAX and then
  wraps (TCB implements this; TCA fires OVF immediately via `rel = 1`).
- USART frame length for 9-bit / parity / 2-stop configurations.

## Suggested regression tests

Each confirmed item has a small host-only repro that fits
`tests/test_avrxt_engine.c`:
1. Raise/clear vector A ×70 with I=0, then raise B: B must dispatch.
2. Dispatch an LVL0 vector, `avr_reset()`: `CPUINT.STATUS == 0` and a new
   interrupt dispatches.
3. PA0 input, `PIN0CTRL.INVEN=1`: four `PORTA.IN` reads must match.
4. TCB0 PWM8 CCMPL=10 over a `rjmp .-2` field: CAPT count ≈ cycles / 11.

## Addendum: found after the review

- **Flash-in-data reads wrong on 48 KB parts (High, fixed in `df4f262`).**
  `_avr_get_ram()` wrapped mapped-flash offsets with `& flashend`, which
  aliases on the non-power-of-two ATmega4808/4809. Data 0xB424 read flash
  0x3424 instead of 0x7424. The Csmith fuzzer found it (`tests/fuzz/README.md`,
  "Findings"). The wrap is now a modulo, with a regression check in
  `test_avrxt_engine.c`.
