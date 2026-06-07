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
| AC (Analog Comparator) | [F] | hysteresis; low-power / run-standby timing; physical pin-level behavior | `avr_ac.c` |
| DAC | [F] | output-buffer behavior; run-standby; reference behavior (only digital→mV) | `avr_dac.c` |
| CCL | [F] | event/peripheral INSEL sources decoded per family (tinyAVR-1 / megaAVR-0 maps verified across DS40002204/05/72/73/74/88/2287) and resolved from cached levels. Live wiring in the core templates: **AC0-2 OUT** (tinyAVR-1) and **AC0 OUT** (megaAVR-0) and **TCD0 WOA/WOB** (tinyAVR-1) now auto-connect to the CCL sources. **Not yet wired:** TCA0 WO0-2, TCB0-2 WO, USART TXD/XCK, SPI lines (need new waveform/line-level output IRQs in those models), and EVSYS EVENT0/1 (CCL not yet an EVSYS user); filter variants; sequencer corner cases; `tick_ctx` typing | `avr_ccl.c`, `sim_tinyx1.h`, `sim_megax08.h` |
| TCD | [F] | clock source approximated as CLK_PER (no dedicated/PLL clock or its prescale); 4 WGM modes work | `avr_tcd.c` |
| RTC / PIT | [F] | SYNCBUSY / PITSTATUS sync bits simplified; CRYSTERR & external-clock pins not modelled; CLK_PER change not retro-applied until reconfig | `avr_rtc.c` |
| BOD / VLM | [F] | brown-out **reset** effect not modelled (VLM voltage monitor is modelled) | `avr_bod.c` |
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
2. **TCD clock source** — model the dedicated TCD clock / prescale instead of
   CLK_PER, so TCD periods match firmware expectations. All 15 tinyAVR-1. [F]
3. **AC hysteresis + run-standby** — needed for realistic comparator firmware.
   All 23 micros. [F]
4. **DAC output-buffer / reference behavior** — all 15 tinyAVR-1. [F]
5. **RTC SYNCBUSY/PITSTATUS + external-clock pins** — all 23 micros. [F]
6. **BOD brown-out reset** — all 23 micros. [F]
7. Polish [P]: USART line-level timing, SPI pin contention, TCB first-period
   scheduling, EVSYS generator-source encodings, ADC exact timing.
