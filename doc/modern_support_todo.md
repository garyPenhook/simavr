# Modern-AVR support TODO — per-micro module gaps

Audit of the 23 modern cores in this fork (15 tinyAVR® 1-series + 8 megaAVR® 0-series).
Every top-level peripheral block on these parts is *present and functional*; the
items below are **feature-depth gaps inside existing models**, not missing blocks.
A gap applies to a micro only if that micro fits the instance.

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
| TCA0 | [P] | single-slope PWM waveform output (WO0-2) now modelled (set at BOTTOM, cleared on the CMPn match; CMPn=0 → static low, CMPn>TOP → static high) and wired to CCL. **Event counting (EVCTRL.CNTEI + EVACT) now modelled** and wired to EVSYS SYNCUSER0: POSEDGE/ANYEDGE clock the counter from event edges (clock scheduler suspended), HIGHLVL gates the prescaled clock on the event line, UPDOWN runs the up-count half. *Remaining:* UPDOWN down-count half (event line high) is not representable by the single-slope up-counter (counter freezes there); FRQ (TOP=CMP0) and the dual-slope WGMODE variants are not modelled — the counter engine is a single-slope up-counter, so both the count behaviour and WOn stay single-slope/low there; split (dual 8-bit) mode; physical WO pins via PORTMUX | `avr_tca.c` |
| EVSYS | [F] | **TCA0 (SYNCUSER0 / USERTCA0) is now wired** in both templates to the new TCA EV_IN input, so event-driven TCA0 counting/gating behaves (see the TCA0 row). The megaAVR-0 user-index map was also corrected: USERTCB0-3 had been wired at indices 0-3 (which alias USERCCLLUT0A..1B); they are now at the real 20-23, and USERTCA0 at 19 (`iom4809.h` EVSYS_t). **Still not connected:** USART (SYNCUSER1 / USERUSART0) — the modern USART stops at BAUD with no EVCTRL / event-input path (`avr_usart_modern.h:42`), and its only event use is IrDA RX-via-event, which needs a bit/line-level RX decode the byte/FIFO USART model does not have (overlaps the USART [P] line-timing gap). Generator-source encodings also still need a datasheet pass. | `avr_evsys.c`, `avr_usart_modern.c`, `sim_tinyx1.h`, `sim_megax08.h` |
| ADC | [P] | conversion delay is a cycle approximation, not exact ADC-clock timing | `avr_adc_modern.c` |
| CPUINT | [F] | **IVSEL is read-back-only** — the bit is stored but the vector table is *not* relocated to the boot section (`avr_cpuint.h:22`). On real tinyAVR-1/megaAVR-0 silicon IVSEL relocates the vector base; a bootloader (or test) that sets IVSEL and relies on relocated vectors will read the bit back correctly and then dispatch from the wrong addresses. LVL0/1, NMI, round-robin, LVL0PRI and CVT are fully modelled. | `avr_cpuint.c` |
| VREF | [P] | **ADC1/AC1/AC2 references now wired.** On tinyAVR-1 16K/32K parts the model publishes ADC1_MV (CTRLC.ADC1REFSEL→ADC1), DAC1_MV (CTRLC.DAC1REFSEL→AC1, DAC1 absent) and DAC2_MV (CTRLD.DAC2REFSEL→AC2, DAC2 absent), and `sim_tinyx1.h` wires each to the matching block (gated on the ADC1/AC1/AC2 fit). Register map verified against DS40002205A 18.4-18.5.3 (p.162-165): CTRLC = ADC1REFSEL[6:4]+DAC1REFSEL[2:0], CTRLD = DAC2REFSEL[2:0]; host-tested in `test_avrxt_engine.c`. The stale "does not exist on the ATtiny3217" comments are corrected. *Remaining (polish):* CTRLB force-enable bits have no power/timing effect (no power model). | `avr_vref.c`, `sim_tinyx1.h` |

**Fully supported (no known gaps):** CLKCTRL, RSTCTRL, SLPCTRL, PORT/VPORT,
PORTMUX, TWI0, NVMCTRL (EEPROM + flash self-program), WDT, CRCSCAN,
SYSCFG/SIGROW (but see the SIGROW/USERROW/FUSE config-region gaps under "Not
implemented at all"), BOD/VLM (voltage-level monitor + brown-out reset →
RSTFR.BORF; only the power/sleep-fidelity aspects shared by all peripherals are
unmodelled).

**Previously listed as gap-free but NOT (see the [F] rows above):** CPUINT
(IVSEL vector relocation is read-back-only). VREF was in this list but the
ADC1/AC1/AC2 references are now wired (downgraded to [P]).

## Not implemented at all

**There are no entirely-unimplemented top-level register-mapped peripherals.**
Every block declared in the device headers and fitted by the templates
(CLKCTRL, RSTCTRL, SLPCTRL, PORT/VPORT, PORTMUX, TCA0, TCB0–3, TCD0, RTC/PIT,
USART0–3, SPI0, TWI0, ADC0/1, AC0–2, DAC0, VREF, NVMCTRL, CCL, EVSYS, WDT,
CRCSCAN, BOD/VLM, SYSCFG, CPUINT) has a model. What remains unimplemented are
NVM/identity **config regions** and one header-less silicon block:

| Region / block | Addr | Status — what is NOT implemented | Affects |
|---|---|---|---|
| USERROW | 0x1300 | User signature row: RAM-backed only — no NVMCTRL write/erase semantics and **not preserved across reset** (persist range covers EEPROM only). Reads/writes hit data[] but behave like plain RAM. | all 23 |
| FUSE read-back window | 0x1280 | Fuses live in `avr->fuse[]` and are consumed internally (BOD/CLKCTRL/CRCSCAN) and writable via NVMCTRL `FUSEWRITE`, but are **not mirrored into the data-space FUSE registers** — a direct firmware read of `FUSE.OSCCFG`/`BODCFG`/etc. returns 0. | all 23 |
| LOCKBIT | 0x128A | Not modelled; reads 0. | all 23 |
| SIGROW SERNUM / OSCnnERR | 0x1100+ | Only `DEVICEID[2:0]` and `TEMPSENSE0/1` are populated; the serial number (`SERNUM0..9`) and oscillator-error rows (`OSC16ERR*`, `OSC20ERR*`) read 0. | all 23 |
| PTC (Peripheral Touch Controller) | — | Present on tinyAVR-1 silicon but **absent from the avr-libc headers**, so it has no register map and no model (cannot be header-driven). | 15 tinyAVR-1 |

Note: the in-tree `iotn3217.h` over-declares `DAC1`/`DAC2` (0x06A8/0x06B0) that do
not exist on the hardware (the tinyAVR 1-series has a single 8-bit DAC0); they are
intentionally not wired.

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
| attiny1614 | A,B | 3 | 2 | 2 | AC×3, ADC×2, TCB×2 |
| attiny1616 | A,B,C | 3 | 2 | 2 | AC×3, ADC×2, TCB×2 |
| attiny1617 | A,B,C | 3 | 2 | 2 | AC×3, ADC×2, TCB×2 |
| attiny3214 | A,B,C | 3 | 2 | 2 | AC×3, ADC×2, TCB×2 |
| attiny3216 | A,B,C | 3 | 2 | 2 | AC×3, ADC×2, TCB×2 |
| attiny3217 | A,B,C | 3 | 2 | 2 | AC×3, ADC×2, TCB×2 |

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
8. **CPUINT IVSEL vector relocation** *(new [F])* — relocate the dispatch base
   when IVSEL is set (needs a boot-section notion in the flash/vector model).
   Affects bootloaders and any firmware relying on relocated vectors. All 23.
9. **VREF ADC1/AC1/AC2 reference** — *done.* The model now publishes ADC1_MV,
   DAC1_MV and DAC2_MV, decoded from CTRLC (ADC1REFSEL[6:4]/DAC1REFSEL[2:0]) and
   CTRLD (DAC2REFSEL[2:0]) per DS40002205A 18.4-18.5.3 (p.162-165); `sim_tinyx1.h`
   wires ADC1, AC1 and AC2 (each gated on its fit). Stale "does not exist on the
   ATtiny3217" comments corrected; host-tested in `test_avrxt_engine.c`.
   tinyAVR-1 16K/32K parts only. [P]
10. **NVM/identity config regions** *(existing [F], see "Not implemented at all")*
   — mirror the FUSE read-back window, make USERROW reset-persistent with
   NVMCTRL write/erase semantics, and populate SIGROW SERNUM/OSCnnERR. Firmware
   reading oscillator calibration, serial number, or fuse bytes gets bad data.
   All 23.
11. Polish [P]: USART line-level timing, SPI pin contention, TCB first-period
   scheduling, EVSYS generator-source encodings, ADC exact timing.
