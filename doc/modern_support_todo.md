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
| CCL | [F] | event/peripheral INSEL sources decoded per family (tinyAVR-1 / megaAVR-0 maps verified across DS40002204/05/72/73/74/88/2287) and resolved from cached levels. Live wiring in the core templates: **AC0-2 OUT** (tinyAVR-1) and **AC0 OUT** (megaAVR-0) and **TCD0 WOA/WOB** (tinyAVR-1) now auto-connect to the CCL sources. **Not yet wired:** TCA0 WO0-2, TCB0-2 WO, USART TXD/XCK, SPI lines (need new waveform/line-level output IRQs in those models), and EVSYS EVENT0/1 (CCL not yet an EVSYS user); filter variants; sequencer corner cases; `tick_ctx` typing | `avr_ccl.c`, `sim_tinyx1.h`, `sim_megax08.h` |
| TCD | [P] | clock source now decoded from CTRLA.CLKSEL — **OSC20M** (unprescaled internal osc, from OSCCFG fuse) and **SYSCLK** (CLK_PER) modelled, scaled by CLK_PER/f_TCD; 4 WGM modes work. *Remaining:* no EXTCLK pin and no dedicated/PLL clock; sub-CLK_PER count resolution not representable (rounded/clamped to ≥1 cycle/count) | `avr_tcd.c` |
| RTC / PIT | [P] | STATUS (CTRLA/CNT/PER/CMP) & PITSTATUS (CTRLBUSY) sync-busy bits now asserted for the documented 2-RTC-clock-cycle latency, so busy-polls spin realistically. *Remaining:* CRYSTERR & external-clock pins not modelled; CLK_PER change not retro-applied until reconfig; write-during-busy not blocked | `avr_rtc.c` |
| BOD / VLM | ✓ | brown-out **reset** modelled: VDD below CTRLB.LVL while enabled invokes a handler the cores wire to RSTCTRL (resets, records RSTFR.BORF); VLM monitor also modelled. Host-tested. No known functional gap | `avr_bod.c` |
| USART | [P] | exact one-wire / line-level timing (async TX/RX, sync timing, loopback all work) | `avr_usart_modern.c` |
| SPI | [P] | pin-contention / electrical realism (buffered protocol is complete) | `avr_spi_modern.c` |
| TCB | [P] | first-period scheduling when enabled with non-zero CNT; filter/edge callback cost on static inputs | `avr_tcb.c` |
| EVSYS | [P] | generator source encodings need a datasheet pass | `avr_evsys.c` |
| ADC | [P] | conversion delay is a cycle approximation, not exact ADC-clock timing | `avr_adc_modern.c` |

**Fully supported (no known gaps):** CLKCTRL, RSTCTRL, SLPCTRL, PORT/VPORT,
PORTMUX, TCA0, TWI0, NVMCTRL (EEPROM + flash self-program), WDT, CRCSCAN,
SYSCFG/SIGROW, VREF, CPUINT (incl. LVL0/1, NMI, round-robin, LVL0PRI, CVT).

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

Base set on **every** modern micro (so every micro carries the AC/CCL/RTC/BOD/
USART/SPI/TCB/EVSYS/ADC gaps at least once): TCA0, TCB0, RTC+PIT, USART0, SPI0,
TWI0, AC0, ADC0, CCL, EVSYS, plus the fully-supported blocks above.

### tinyAVR 1-series (15) — additionally fit **DAC0 and TCD0 on every part**

So **all 15 tinyAVR-1 micros carry, at minimum, the AC + DAC + CCL + TCD + RTC +
BOD + USART + SPI + TCB + EVSYS + ADC gaps.** Instance counts that multiply a gap:

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

So **all 8 megaAVR-0 micros carry the AC + CCL + RTC + BOD + USART + SPI + TCB +
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
   **AC** (all 23) and **TCD0** (tinyAVR-1) — these track the real peripherals
   end-to-end. *Remaining:* TCA0 WO0-2 and TCB0-2 WO need new waveform-output
   level IRQs in `avr_tca.c` / `avr_tcb.c` before they can be wired; USART
   TXD/XCK and SPI SCK/MOSI/MISO need line-level output IRQs (overlaps the USART
   [P] line-timing gap); EVSYS EVENT0/1 need the CCL added as an EVSYS user.
   [F]
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
7. Polish [P]: USART line-level timing, SPI pin contention, TCB first-period
   scheduling, EVSYS generator-source encodings, ADC exact timing.
