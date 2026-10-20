# Modern-AVR support TODO — per-micro module gaps

Audit of the 23 modern cores in this fork (15 tinyAVR® 1-series + 8 megaAVR® 0-series),
plus the 11 tinyAVR® 0-series cores added 2026-10-19 (ATtiny202/204/402/404/406/804/806/807/
1604/1606/1607). The 0-series reuses `sim_tinyx1.h` and fits a strict subset of the 1-series
blocks (no DAC, TCD, PTC; one TCB/AC/ADC — AN2636 table 2-1, DS40002312A/DS40002318A); its
VREF.CTRLA.DAC0REFSEL is the AC0 reference. The "all 23" counts below predate it and apply to
those 23 cores; each 0-series part inherits the gaps of the blocks it fits.
The 12 tinyAVR® 2-series cores (ATtiny424/426/427/824/826/827/1624/1626/1627/3224/3226/3227)
were added 2026-10-19 on a new template, `sim_tinyx2.h`; their gaps are listed in the
[tinyAVR 2-series section](#tinyavr-2-series-12-cores-added-2026-10-19) at the end.
Every top-level peripheral block on these parts is *present and functional*; the
items below are **feature-depth gaps inside existing models**, not missing blocks.
A gap applies to a micro only if that micro fits the instance.

For a separate roadmap for a Python GUI frontend layered on the native simulator,
see [`python_gui_todo.md`](python_gui_todo.md).

Verified 2026-06-07 against the in-tree models, the README backlog, and the
ATtiny3216/17 datasheet (DS40002205A). Source of truth for "fitted instances" is
each device's avr-libc header as evaluated by the core templates
(`sim_tinyx1.h`, `sim_megax08.h`).

Legend: **[F]** = functional gap (firmware can observe wrong/absent behavior);
**[P]** = fidelity/polish (rarely hit by real firmware).

## Partial modules (shared model, applies to every micro that fits it)

| Module | Sev | Gap (what is NOT fully supported) | Model |
|---|---|---|---|
| AC (Analog Comparator) | [P] | input hysteresis (HYSMODE ±10/±25/±50 mV) now modelled with a held-state band; *remaining:* low-power / run-standby power+timing (no power or sleep-mode-gating model), physical output-pin buffer (OUTEN) | `avr_ac.c` |
| DAC | [P] | output buffer (OUTEN) now modelled: a separate pin output, gated by ENABLE+OUTEN, distinct from the ENABLE-only internal OUT to AC/ADC. *Remaining:* run-standby power/timing (no model hook), output-buffer start-up time; conversion stays the ideal digital→mV | `avr_dac.c` |
| CCL | [P] | event/peripheral INSEL sources decoded per family (tinyAVR-1 / megaAVR-0 maps verified across DS40002204/05/72/73/74/88/2287) and resolved from cached levels. Live wiring in the core templates: **AC0-2 OUT** (tinyAVR-1) and **AC0 OUT** (megaAVR-0), **TCD0 WOA/WOB** (tinyAVR-1), **TCA0 WO0-2 single-slope PWM** (all 23), and **TCB0-2 WO 8-bit PWM** (TCB0/1 tinyAVR-1, TCB0-2 megaAVR-0) now auto-connect to the CCL sources. **Not yet wired:** USART TXD/XCK and SPI lines (need line-level output IRQs in those models), and EVSYS EVENT0/1 (CCL not yet an EVSYS user); filter variants; sequencer corner cases; `tick_ctx` typing | `avr_ccl.c`, `sim_tinyx1.h`, `sim_megax08.h` |
| TCD | [P] | clock source now decoded from CTRLA.CLKSEL — **OSC20M** (unprescaled internal osc, from OSCCFG fuse) and **SYSCLK** (CLK_PER) modelled, scaled by CLK_PER/f_TCD; 4 WGM modes work. *Remaining:* no EXTCLK pin and no dedicated/PLL clock; sub-CLK_PER count resolution not representable (rounded/clamped to ≥1 cycle/count) | `avr_tcd.c` |
| RTC / PIT | [P] | STATUS (CTRLA/CNT/PER/CMP) & PITSTATUS (CTRLBUSY) sync-busy bits now asserted for the documented 2-RTC-clock-cycle latency, so busy-polls spin realistically. *Remaining:* CRYSTERR & external-clock pins not modelled; CLK_PER change not retro-applied until reconfig; write-during-busy not blocked | `avr_rtc.c` |
| USART | [P] | exact one-wire / line-level timing (async TX/RX, sync timing, loopback all work) | `avr_usart_modern.c` |
| SPI | [P] | pin-contention / electrical realism (buffered protocol is complete) | `avr_spi_modern.c` |
| TCB | [P] | 8-bit PWM (PWM8) waveform output now modelled (set at BOTTOM, cleared at CCMPH; CAPT per period) and wired to CCL. *Remaining:* Single-Shot mode (the other WO-producing mode, event-triggered one-shot pulse) is not modelled — its WO stays low; first-period scheduling when enabled with non-zero CNT; filter/edge callback cost on static inputs | `avr_tcb.c` |
| TCA0 | [P] | single-slope PWM waveform output (WO0-2) now modelled (set at BOTTOM, cleared on the CMPn match; CMPn=0 → static low, CMPn>TOP → static high) and wired to CCL. **Event counting (EVCTRL.CNTEI + EVACT) now modelled** and wired to EVSYS SYNCUSER0: POSEDGE/ANYEDGE clock the counter from event edges (clock scheduler suspended), HIGHLVL gates the prescaled clock on the event line, UPDOWN runs the up-count half. *Remaining:* UPDOWN down-count half (event line high) is not representable by the single-slope up-counter (counter freezes there); FRQ (TOP=CMP0) and the dual-slope WGMODE variants are not modelled — the counter engine is a single-slope up-counter, so both the count behaviour and WOn stay single-slope/low there; split (dual 8-bit) mode; physical WO pins via PORTMUX; **CTRLECLR/CTRLESET (offset 0x06/0x07) is not modelled** — the CMD field (RESTART/RESET) and the DIR direction bit are treated as plain RAM, so a firmware-issued counter RESTART/RESET command has no effect (audit 2026-06-07) | `avr_tca.c` |
| EVSYS | [F] | **TCA0 (SYNCUSER0 / USERTCA0) is now wired** in both templates to the new TCA EV_IN input, so event-driven TCA0 counting/gating behaves (see the TCA0 row). The megaAVR-0 user-index map was also corrected: USERTCB0-3 had been wired at indices 0-3 (which alias USERCCLLUT0A..1B); they are now at the real 20-23, and USERTCA0 at 19 (`iom4809.h` EVSYS_t). **Still not connected:** USART (SYNCUSER1 / USERUSART0) — the modern USART stops at BAUD with no EVCTRL / event-input path (`avr_usart_modern.h:42`), and its only event use is IrDA RX-via-event, which needs a bit/line-level RX decode the byte/FIFO USART model does not have (overlaps the USART [P] line-timing gap). Generator-source encodings also still need a datasheet pass. | `avr_evsys.c`, `avr_usart_modern.c`, `sim_tinyx1.h`, `sim_megax08.h` |
| ADC | [P] | conversion delay is a cycle approximation, not exact ADC-clock timing | `avr_adc_modern.c` |
| CPUINT | ✓ | **IVSEL vector relocation now modelled.** The engine adds a vector base in `avr_service_interrupts_modern()`: IVSEL=1 → boot section (flash 0x0000), IVSEL=0 → application section (flash FUSE.BOOTEND*256), per DS40002205A 13.5.1. The CPUINT register block mirrors IVSEL into the engine and the cores pass the BOOTEND fuse index (8); with the default BOOTEND=0 the base stays 0, so the common no-bootloader case is unchanged. Host-tested in `test_avrxt_engine.c`. LVL0/1, NMI, round-robin, LVL0PRI and CVT were already fully modelled. **Pending-queue leak fixed (2026-09-24):** clearing a queued, unserviced vector now dequeues it, so repeated flag clears can no longer fill the 64-entry FIFO and lose later interrupts (`doc/code_review_2026-09-24.md` #1). | `avr_cpuint.c`, `sim_interrupts.c` |
| VREF | [P] | **ADC1/AC1/AC2 references now wired.** On tinyAVR-1 16K/32K parts the model publishes ADC1_MV (CTRLC.ADC1REFSEL→ADC1), DAC1_MV (CTRLC.DAC1REFSEL→DAC1/AC1) and DAC2_MV (CTRLD.DAC2REFSEL→DAC2/AC2), and `sim_tinyx1.h` wires each to the matching block (gated on the ADC1/AC1/AC2 fit). **DAC1 (0x06A8) and DAC2 (0x06B0) are now modelled** (DS40002205A Table 7-1) — full DAC blocks with no output pin whose DATA-scaled output is the AC1/AC2 "DAC" negative input (31.3.2.3), wired like DAC0→AC0; the prior "DAC1/DAC2 absent" assumption was wrong. Register map verified against DS40002205A 18.4-18.5.3 (p.162-165): CTRLC = ADC1REFSEL[6:4]+DAC1REFSEL[2:0], CTRLD = DAC2REFSEL[2:0]; host-tested in `test_avrxt_engine.c`. *Remaining (polish):* CTRLB force-enable bits have no power/timing effect (no power model). | `avr_vref.c`, `sim_tinyx1.h` |
| PORTMUX | [P] | **Routing is implemented for a first slice, not the whole chip.** `avr_port_modern` has a per-pin peripheral override (`avr_port_modern_override`: replaces the OUT level, optionally forces output; DIR/OUT never modified; INVEN still inverts) and `avr_portmux` maps a routing field to a pin per routed function (`avr_portmux_add_func` + `avr_portmux_bind_irq`), releasing the old pin when the field changes. Opt-in per device via `SIM_PORTMUX_*` tables in the core `.c`. **Routed so far (ATtiny3216/3217 only, pins from DS40002205A Table 5-1):** TCA0 WO0-2 (override when CMPnEN set in a waveform mode; user sets DIR, DS 20.3.3.4.5) and USART0 TXD (real Start/data/parity/Stop waveform on the pin from CTRLC; TXEN overrides, user sets DIR; 9-bit frames sent as 8, ODME and sync mode not modelled, 64-byte pin queue). **Not routed:** USART RXD/XCK, SPI, TWI, TCA split mode WO3-5, TCB/TCD outputs, CCL LUT outputs, EVOUT, other tinyAVR 0/1 devices (their pin tables differ and need checking per data sheet), tinyAVR 2-series and megaAVR-0 (different ROUTEA registers). Those fields are stored and displayed only. | `avr_portmux.c`, `avr_port_modern.c`, `avr_tca.c`, `avr_usart_modern.c`, `sim_tinyx1.h`, `sim_tiny3216.c`, `sim_tiny3217.c` |

**Fully supported (no known gaps):** CLKCTRL, RSTCTRL, SLPCTRL, PORT/VPORT
(incl. per-pin PINnCTRL pull-up / INVEN, host-tested), TWI0, NVMCTRL (EEPROM +
flash self-program), WDT, CRCSCAN,
SYSCFG/SIGROW (but see the SIGROW/USERROW/FUSE config-region gaps under "Not
implemented at all"), BOD/VLM (voltage-level monitor + brown-out reset →
RSTFR.BORF; only the power/sleep-fidelity aspects shared by all peripherals are
unmodelled).

**Previously listed as gap-free but NOT, now fixed:** CPUINT (IVSEL vector
relocation is now modelled) and VREF (ADC1/AC1/AC2 references now wired,
downgraded to [P]).

## Not implemented at all

**There are no entirely-unimplemented top-level register-mapped peripherals.**
Every block declared in the device headers and fitted by the templates
(CLKCTRL, RSTCTRL, SLPCTRL, PORT/VPORT, PORTMUX, TCA0, TCB0–3, TCD0, RTC/PIT,
USART0–3, SPI0, TWI0, ADC0/1, AC0–2, DAC0, VREF, NVMCTRL, CCL, EVSYS, WDT,
CRCSCAN, BOD/VLM, SYSCFG, CPUINT) has a model. The NVM/identity **config
regions** (USERROW, FUSE window, LOCKBIT, SIGROW SERNUM/OSCnnERR) are now
modelled too; the only remaining unmodelled silicon is one header-less block:

| Region / block | Addr | Status — what is NOT implemented | Affects |
|---|---|---|---|
| USERROW | 0x1300 | **Done.** Modelled as "one extra page of EEPROM" (DS40002205A 6.6): writes load the shared NVM page buffer and commit via the EEPROM commands (PAGEWRITE/PAGEERASE/PAGEERASEWRITE/PAGEBUFCLR, EEBUSY/EEREADY); it persists across reset (second persist range) and is **not** affected by CHIPERASE. Per-device size (32B on small parts, 64B on 16K/32K & megaAVR-0). Host-tested. | all 23 |
| FUSE read-back window | 0x1280 | **Done.** `avr_syscfg` exposes the 9-byte FUSE_t window with a live read handler returning `avr->fuse[]` (DS40002205A 6.10: fuses are CPU-readable, not CPU-writable), so `FUSE.OSCCFG`/`BODCFG`/`SYSCFG0`/`BOOTEND` etc. read the real fuse value and reflect any NVMCTRL `FUSEWRITE`; direct writes are ignored. Host-tested. *Caveat (audit 2026-06-07):* `avr->fuse[]` is not seeded with silicon factory defaults — an unset fuse reads 0, so e.g. `FUSE.SYSCFG0` reads `0x00` rather than the factory `0xC4` unless the ELF's `.fuse` section sets it. Functionally handled where it matters (CRCSCAN treats all-zero SYSCFG0 as NOCRC). | all 23 |
| LOCKBIT | 0x128A | **Done.** Reads the unlocked key `0xC5` (DS40002205A 6.10.4.9), read-only to the CPU. simavr does not model the UPDI debug-access lock (CPU access is always permitted), so this is a faithful read-back. Host-tested. | all 23 |
| SIGROW SERNUM / OSCnnERR | 0x1100+ | **Done.** `SERNUM0..9` is populated with a deterministic non-zero placeholder (no canonical value exists; real silicon is never all-zero). `OSC16ERR*`/`OSC20ERR*` are signed frequency-error calibrations where **0 = no error**, so they are left at 0 (a valid value). `DEVICEID[2:0]` and `TEMPSENSE0/1` were already populated. Host-tested. | all 23 |
| PTC (Peripheral Touch Controller) | — | Present on tinyAVR-1 silicon but **absent from the avr-libc headers**, so it has no register map and no model (cannot be header-driven). | 15 tinyAVR-1 |

Note: `DAC1`/`DAC2` (0x06A8/0x06B0) are real DAC blocks on the 16K/32K tinyAVR-1
parts (1614/1616/1617/3214/3216/3217), per DS40002205A Table 7-1 and the device
headers — *not* spurious. They have no output pin; their DATA-scaled output is
the AC1/AC2 "DAC" negative input (DS40002205A 31.3.2.3). **Done:** wired in
`sim_tinyx1.h` (gated on `AC1_AC_vect_num`/`AC2_AC_vect_num`), mirroring the
DAC0→AC0 routing; VREF.CTRLC/CTRLD select their reference. Host-tested. The 9
smaller parts have only DAC0.

## Per-micro fitted instances and applicable gaps

Base set on **every** modern micro (so every micro carries the AC/CCL/RTC/
USART/SPI/TCB/EVSYS/ADC gaps at least once): TCA0, TCB0, RTC+PIT, USART0, SPI0,
TWI0, AC0, ADC0, CCL, EVSYS, plus the fully-supported blocks above (incl.
BOD/VLM, now gap-free).

### tinyAVR 1-series (15) — additionally fit **DAC0 and TCD0 on every part**

So **all 15 tinyAVR-1 micros carry, at minimum, the AC + DAC + CCL + TCD + RTC +
USART + SPI + TCB + EVSYS + ADC gaps.** Instance counts that multiply a gap:

| Micro | Ports | AC | ADC | TCB | Extra gap multiplier |
|---|---|---|---|---|---|
| attiny212 | A | 1 | 1 | 1 | — |
| attiny412 | A | 1 | 1 | 1 | — |
| attiny214 | A,B | 1 | 1 | 1 | — |
| attiny414 | A,B | 1 | 1 | 1 | — |
| attiny814 | A,B | 1 | 1 | 1 | — |
| attiny416 | A,B,C | 1 | 1 | 1 | — |
| attiny417 | A,B,C | 1 | 1 | 1 | — |
| attiny816 | A,B,C | 1 | 1 | 1 | — |
| attiny817 | A,B,C | 1 | 1 | 1 | — |
| attiny1614 | A,B | 3 | 2 | 2 | AC×3, ADC×2, TCB×2, DAC×3 |
| attiny1616 | A,B,C | 3 | 2 | 2 | AC×3, ADC×2, TCB×2, DAC×3 |
| attiny1617 | A,B,C | 3 | 2 | 2 | AC×3, ADC×2, TCB×2, DAC×3 |
| attiny3214 | A,B,C | 3 | 2 | 2 | AC×3, ADC×2, TCB×2, DAC×3 |
| attiny3216 | A,B,C | 3 | 2 | 2 | AC×3, ADC×2, TCB×2, DAC×3 |
| attiny3217 | A,B,C | 3 | 2 | 2 | AC×3, ADC×2, TCB×2, DAC×3 |

### megaAVR 0-series (8) — **no DAC, no TCD** (those gaps do not apply)

So **all 8 megaAVR-0 micros carry the AC + CCL + RTC + USART + SPI + TCB +
EVSYS + ADC gaps** (1× AC0, 1× ADC0), multiplied by USART/TCB instance count:

| Micro | Ports | USART | TCB | Notes |
|---|---|---|---|---|
| atmega808 | A–F | 0,1,2 | 0,1,2 | USART gap ×3, TCB gap ×3 |
| atmega1608 | A–F | 0,1,2 | 0,1,2 | USART gap ×3, TCB gap ×3 |
| atmega3208 | A–F | 0,1,2 | 0,1,2 | USART gap ×3, TCB gap ×3 |
| atmega4808 | A–F | 0,1,2 | 0,1,2 | USART gap ×3, TCB gap ×3 |
| atmega809 | A–F | 0,1,2,3 | 0,1,2,3 | USART gap ×4, TCB gap ×4 |
| atmega1609 | A–F | 0,1,2,3 | 0,1,2,3 | USART gap ×4, TCB gap ×4 |
| atmega3209 | A–F | 0,1,2,3 | 0,1,2,3 | USART gap ×4, TCB gap ×4 |
| atmega4809 | A–F | 0,1,2,3 | 0,1,2,3 | USART gap ×4, TCB gap ×4 |

## Suggested work order (highest firmware impact first)

1. **CCL event/peripheral input sources** — INSEL decode + source-level cache
   done (per-family, datasheet-verified, host-tested). Live auto-wiring done for
   **AC** (all 23), **TCD0** (tinyAVR-1), **TCA0 WO0-2** (all 23), and **TCB0-2
   WO** (tinyAVR-1 TCB0/1, megaAVR-0 TCB0-2) — these track the real peripherals
   end-to-end. TCA0 emits single-slope PWM levels (DS40002205A 20.3.3.4.3: set
   at BOTTOM, cleared on CMPn match); TCB emits 8-bit-PWM levels (21.3.3.1.8:
   set at BOTTOM, cleared at CCMPH) on new WO output IRQs, both wired into the
   CCL source MUX (host-tested). *Remaining (all [P]):* TCB Single-Shot WO and
   TCA0 FRQ / dual-slope WO need their counter paths modelled first (TCA0 FRQ
   uses TOP=CMP0 and dual-slope down-counts; Single-Shot is event-triggered);
   USART TXD/XCK and SPI SCK/MOSI/MISO need line-level output IRQs (overlaps the
   USART [P] line-timing gap); EVSYS EVENT0/1 need the CCL added as an EVSYS
   user.
2. **TCD clock source** — *done.* CTRLA.CLKSEL is decoded: OSC20M (unprescaled
   internal oscillator, resolved from the OSCCFG fuse) and SYSCLK (CLK_PER) are
   modelled, with the schedule scaled by CLK_PER/f_TCD so a prescaled main clock
   no longer drags an OSC20M-clocked TCD. Host-tested in `test_avrxt_engine.c`.
   *Remaining (downgraded to [P]):* EXTCLK pin and a dedicated/PLL TCD clock;
   sub-CLK_PER count resolution. All 15 tinyAVR-1. [P]
3. **AC hysteresis** — *done.* CTRLA.HYSMODE (±10/±25/±50 mV) modelled as a
   held-state input band; host-tested in `test_avrxt_engine.c`. Run-standby /
   low-power are power/timing fidelity with no model hook (the simulator has no
   power model and only a shallow sleep model), so they stay unmodelled. All 23
   micros. [P]
4. **DAC output buffer** — *done.* CTRLA.OUTEN now gates a distinct pin output
   (ENABLE+OUTEN), separate from the ENABLE-only internal OUT that feeds AC/ADC
   (DS40002205A 31.3.2.3); host-tested. Run-standby/start-up timing stay
   unmodelled (no power model). All 15 tinyAVR-1. [P]
5. **RTC SYNCBUSY/PITSTATUS** — *done.* STATUS (CTRLABUSY/CNTBUSY/PERBUSY/
   CMPBUSY) and PITSTATUS (CTRLBUSY) now assert for the documented 2-RTC-clock
   sync latency (DS40002205A 23.12.2), converted to CPU cycles; host-tested.
   External-clock pins / CRYSTERR remain unmodelled. All 23 micros. [P]
6. **BOD brown-out reset** — *already done* (commit 2476c96, datasheet-verified
   levels, host-tested): VDD below the BOD level invokes a handler the cores
   wire to RSTCTRL, resetting and recording RSTFR.BORF. The stale "not modelled"
   notes have been corrected. All 23 micros. ✓
7. **EVSYS event users for TCA0 / USART** — **TCA0 done.** EVCTRL (CNTEI +
   EVACT) is modelled in `avr_tca.c` and SYNCUSER0=TCA0 is wired to the new
   TCA EV_IN in both templates: POSEDGE/ANYEDGE event-clock the counter,
   HIGHLVL gates the prescaled clock, UPDOWN runs the up-count half
   (DS40002205A 20.5.10); host-tested in `test_avrxt_engine.c`. The megaAVR-0
   USERTCB0-3 indices were corrected (0-3 → 20-23) and USERTCA0 wired at 19.
   *Remaining (re-scoped, blocked on line-level RX):* the USART event user
   (SYNCUSER1/USERUSART0) is **not** wired — its only event use is IrDA
   RX-via-event, which needs a bit/line-level RX decode the byte/FIFO USART
   model lacks; this now folds into the USART [P] line-timing gap (item 11).
   All 23 micros.
8. **CPUINT IVSEL vector relocation** — *done.* The modern dispatch adds a
   vector base (`avr_service_interrupts_modern()`): IVSEL=1 → boot section
   (0x0000), IVSEL=0 → application section (FUSE.BOOTEND*256), per DS40002205A
   13.5.1. The CPUINT register block mirrors IVSEL into the engine and the cores
   pass the BOOTEND fuse index; BOOTEND=0 leaves the base at 0 (no-bootloader
   case unchanged). Host-tested in `test_avrxt_engine.c`. All 23. ✓
9. **VREF ADC1/AC1/AC2 reference** — *done.* The model now publishes ADC1_MV,
   DAC1_MV and DAC2_MV, decoded from CTRLC (ADC1REFSEL[6:4]/DAC1REFSEL[2:0]) and
   CTRLD (DAC2REFSEL[2:0]) per DS40002205A 18.4-18.5.3 (p.162-165); `sim_tinyx1.h`
   wires ADC1, AC1 and AC2 (each gated on its fit). Stale "does not exist on the
   ATtiny3217" comments corrected; host-tested in `test_avrxt_engine.c`.
   tinyAVR-1 16K/32K parts only. [P]
10. **NVM/identity config regions** — *done.* FUSE read-back window
   (`avr_syscfg`, live read of `avr->fuse[]`); USERROW (`avr_nvmctrl`:
   EEPROM-style write/erase via the shared page buffer, reset-persistent,
   CHIPERASE-immune; per-device 32/64 B); LOCKBIT (reads `0xC5` unlocked);
   SIGROW SERNUM (deterministic non-zero placeholder) and OSCnnERR (0 = no
   error, valid). All host-tested across the family templates. All 23. ✓
11. Polish [P]: USART line-level timing, SPI pin contention, TCB first-period
   scheduling, EVSYS generator-source encodings, ADC exact timing.

## Audit findings (2026-06-08)

These are additional codebase problems found during a repository audit. They are
not duplicates of the per-micro modern-AVR feature-depth gaps above.

| Area | Sev | Problem | Evidence |
|---|---|---|---|
| RTC counter reschedule | ✓ | **Fixed (2026-06-08).** `avr_rtc.c` now latches the live counter value into the CNT registers (`rtc_latch_cnt()`) before any CTRLA/CLKSEL/PER/CMP reschedule, so live RTC reconfiguration preserves the true count instead of re-anchoring from the stale register (often 0). Mirrors the TCA latch-before-reschedule pattern. Host-tested ("modern RTC live-reschedule continuity": a CMP moved just ahead of an advanced live count matches ~2 ticks later, not ~12). | `simavr/sim/avr_rtc.c` (`rtc_latch_cnt`, ctrla/clksel/per/cmp write handlers) |
| TWI modern slave | ✓ | **Fixed (2026-06-08).** The modern TWI slave now honours the pre-configured `SCTRLB.ACKACT` (0=ACK, 1=NACK, DS40002205A 26.5.10) on both address match and write-data reception, instead of always emitting `TWI_COND_ACK`. A NACKed address leaves APIF set (firmware still sees the event) but the slave unselected so no data is exchanged; a NACKed write byte is reported to the master via RXACK. In this synchronous model firmware pre-arms ACKACT (the default 0=ACK keeps prior behaviour). Host-tested ("modern TWI0": NACK-on-address + subsequent write dropped). | `simavr/sim/avr_twi_modern.c` (`avr_twi_modern_irq_input`) |
| SPI buffered-mode switch | ✓ | **Fixed (2026-06-08).** `avr_spi_modern_ctrlb_write()` now cancels the scheduled transfer timer and clears `busy` when `CTRLB.BUFEN` toggles, so an in-flight transfer cannot complete later under the new mode and leak a stale `SPI_IRQ_OUTPUT`/interrupt/flag across the boundary. Host-tested ("modern SPI0 BUFEN mode-switch teardown"). | `simavr/sim/avr_spi_modern.c:289-308` |
| Shared I2C helper examples | ✓ | **Fixed (2026-06-08).** `i2c_start_wait()` now bounds its ack-polling at `I2C_START_WAIT_MAX_RETRIES` (default 100) and gives up — leaving the bus stopped — instead of spinning forever on an absent device. Applied to both the shared helper and the board-local copy; the FIXME is removed. | `examples/shared/twimaster.c`, `examples/board_ds1338/twimaster.c` |
| Classic external-interrupt coverage | ✓ | **Fixed (2026-06-08).** Added a dedicated, self-contained regression for the classic low-level (level-triggered) external interrupt: `atmega168_extint_level.c` drives its own INT0/PD2 output low to assert the level, so it exercises the continuous re-trigger path *and* its clean termination (ISR releases the level after 3 entries), plus mask-respect (re-asserting low with INT0 masked produces no interrupt). Kept separate from the two choreographed ioport tests rather than perturbing their exact expected-output strings; their stale TODO comments now point at the new coverage. | `tests/atmega168_extint_level.c`, `tests/test_atmega168_extint_level.c`, `tests/atmega168_ioport.c`, `tests/atmega48_ioport.c` |
| Classic UART pin override | ✓ | **Fixed (2026-06-08).** Modelled RxD/TxD pin-function override. Added an additive pin-function-override layer to `avr_ioport` (per-pin override mask/dir/value, empty by default so every existing core is byte-for-byte unchanged) plus an `AVR_IOCTL_IOPORT_SET_FUNCTION` ioctl; the USART now claims TxD as a high-idle output while TXEN is set and RxD as an input while RXEN is set, overriding GPIO DDR/PORT, via optional `txd`/`rxd` pin coordinates wired in the megaX8 template (ATmega48/88/168/328: TxD=PD1, RxD=PD0). Reconciliation runs only on writes to the TXEN/RXEN control register and from the written bit values — avoiding the reset-time `TXEN` printf convenience — so the frozen ioport tests (which printf without writing UCSRB) keep PD0/PD1 as GPIO. Host firmware test added. | `simavr/sim/avr_uart.c`, `avr_uart.h`, `avr_ioport.c`, `avr_ioport.h`, `cores/sim_megax8.h`, `tests/atmega48_uart_pins.c` |

## tinyAVR 2-series (12 cores, added 2026-10-19)

ATtiny424/426/427, 824/826/827, 1624/1626/1627, 3224/3226/3227 on `sim_tinyx2.h`
(DS40002311B, DS40002234B, DS40002345B). Register bases and layouts were diffed against
the 1-series and megaAVR-0 headers; the blocks that differ got new or extended models:

| Module | New / changed | Model |
|---|---|---|
| ADC0 | **new** 12-bit differential ADC with PGA, series/burst accumulation, window comparator, three vectors (`avr_adc_tiny2.c`) — not the 10-bit `avr_adc_modern.c` | `avr_adc_tiny2.c` |
| AC0 | `avr_ac_init_dacref()`: `DACREF` (reset 0xFF, `VREF*DACREF/256`), `MUXNEG` = AINN0/1/2/DACREF, datasheet VHYS | `avr_ac.c` |
| VREF | `avr_vref_init_tiny2()`: only `AC0REFSEL` (1.024/2.048/2.5/4.096 V/VDD); the ADC picks its reference in `ADC0.CTRLC` | `avr_vref.c` |
| CCL | 4 LUTs on the mega layout, plus the new `INTCTRL0`/`INTFLAGS` LUT-output-edge interrupt (`avr_ccl_set_interrupt()`) | `avr_ccl.c` |
| EVSYS | `avr_evsys_init_xt()`: 6 channels, 20 users (`USERCCLLUTnA/B`…`USERTCB1COUNT`) | `avr_evsys.c` |
| PORTMUX | `avr_portmux_init_n()`: six `*ROUTEA` registers at 0x05E0 | `avr_portmux.c` |

Remaining gaps (all [P] unless noted):

* **ADC0:** `TIMEBASE`, `RUNSTDBY`, `DBGRUN`, `PGABIASSEL` are stored only (no power/clock
  model); `CLK_PER` is taken as the CPU clock (CLKCTRL's peripheral prescaler is ignored, as in
  `avr_adc_modern.c`); `RESULT`/`SAMPLE` update atomically (no TEMP-byte latch between the
  low- and high-byte reads); only the low byte read clears `RESRDY`/`SAMPRDY`; changing the
  mux or reference mid-conversion does not corrupt the result; the LOWLAT "modules stay
  warm" rule is a per-module bitmask, not an analog settling model.
* **EVSYS:** generators wired are AC0 OUT, ADC0 RES/SAMP/WCMP, TCB0/1 CAPT and CCL LUT0-3;
  RTC/PIT, PORT pins, USART XCK, SPI SCK, TCA0 and TCB OVF are **not** generators yet, and
  the channel-availability restrictions (PORT pins, even/odd PIT) are not enforced. Users
  wired are ADC0 START, TCB0/1 CAPT and TCA0 CNTA; `USERCCLLUTnA/B` (the CCL has no per-LUT
  event inputs), `USERTCA0CNTB`, the TCB `COUNT` users and `USERUSARTnIRDA` are **not**.
* **CCL:** USART TXD (INSEL 0x8) and SPI (INSEL 0x9) sources are unwired, as on every
  family (the USART/SPI models expose no line-level output IRQs).
* **PORT:** the 14/20-pin parts fit only PORTB[3:0] / PORTB[5:0], PORTC[3:0]; the model does
  not mask unfitted pins (same as the 1-series).
* **SIGROW:** `OSCCAL16M0/1` and `OSCCAL20M0/1` (0x1118-0x111B) are not populated.

Defects found while doing this work that are **not** specific to the 2-series (left as is):

* **[F] GPIOR0-3 alias r28-r31 on every modern core.** simavr keeps the register file in
  `data[0..31]`; the low-I/O redirect only covers the VPORTs, so firmware that uses
  `GPIOR0..3` (0x1C-0x1F) and also uses `Y`/`Z` corrupts both. `tests/attiny3217_selftest.c`
  only survives because of register allocation; `tests/attiny3224_selftest.c` uses SRAM
  for its result block for this reason.
* `avr_evsys_init_mega()` instantiates 6 channels, but `iom4809.h` defines `CHANNEL0..7`.
* The megaAVR-0 AC uses the tinyAVR-1 model (no `DACREF`, no AINN2); `iom4809.h` defines
  `AC0.DACREF`. The 2-series `avr_ac_init_dacref()` is a candidate once DS40002173C is checked.
* AC hysteresis shape is **unverified**: the datasheets give only a typical `VHYS` per
  `HYSMODE` (2-series: 0/10/30/55 mV, 0/10/40/80 mV in low-power mode; DS40002311B Tables
  33-30/31), not the window geometry. The 1-series model applies its table as a ±band; the
  2-series variant assumes `VHYS` is the total width, i.e. a ±VHYS/2 half-window.

Review notes (2-series, unfixed, all [P]): supply voltage lives in three unsynchronised places
(`avr_vref_t.vdd_mv`, `avr_adc_tiny2_t.vdd_mv`, AC0's cached reference), so changing VDD at
runtime after selecting `AC0REFSEL=VDD` does not refresh AC0; ADC IRQs exist for input codes
the mux cannot select (AIN0/8/9/12-15 are accepted but MUXPOS=0 reads ground); the AC model
honours reserved MUXPOS/MUXNEG codes; every accumulation step flags `SAMPOVR` if `SAMPLE` was not
read (literal reading of the datasheet, not confirmed for series/burst).
