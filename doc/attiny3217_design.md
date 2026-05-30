# Design Plan: Adding ATtiny3217 (modern AVR / AVRxt) support to simavr

Status: Proposal / design. Target device: **ATtiny3217** (tinyAVR 1-series).

---

## 1. Goal & scope

Add a working simavr core for the ATtiny3217 so that firmware built with a
modern avr-gcc + ATtiny_DFP toolchain can be loaded and executed with enough
peripheral fidelity to be useful (GPIO, timers, USART, basic ADC, interrupts).

This is **not** "just another core file". Every existing simavr core
(`sim_mega328.c`, `sim_tiny85.c`, …) is a classic AVR (AVRe/AVRe+). The
ATtiny3217 uses the **AVRxt** core with the **modern peripheral architecture**
(the same family as megaAVR-0, AVR DA/DB). simavr's *engine* bakes in several
classic-AVR assumptions that do not hold for modern AVR. The bulk of the work is
in the engine, not the core descriptor.

---

## 2. Device facts (from `avr/iotn3217.h`, DFP 3.4.278)

| Property | Value |
|---|---|
| Core | AVRxt |
| Flash | 32 KB, `0x0000–0x7FFF`, page 128 B; also mapped into data space at `0x8000` |
| SRAM | 2 KB at `0x3800–0x3FFF` (`RAMEND=0x3FFF`) |
| EEPROM | 256 B at `0x1400`, mapped in data space |
| Signature | `1E 95 22` |
| Fuses | `FUSE_MEMORY_SIZE = 10` (10 fuse bytes, mapped at `0x1280`) |
| CCP register | `0x0034`, signatures `0x9D` (SPM) / `0xD8` (IOREG) |
| SP | data `0x3D/0x3E`; SREG data `0x3F` |
| Interrupt vectors | 34 vectors via the **CPUINT** controller |

### Data-space memory map (the important part)
```
0x0000–0x003F  low I/O (VPORTA..C, GPIOR0..3, CCP, SP, SREG)
0x0040–0x1FFF  extended I/O — all modern peripherals live here:
               RSTCTRL 0x40, SLPCTRL 0x50, CLKCTRL 0x60, BOD 0x80, VREF 0xA0,
               WDT 0x100, CPUINT 0x110, CRCSCAN 0x120, RTC 0x140, EVSYS 0x180,
               CCL 0x1C0, PORTMUX 0x200, PORTA/B/C 0x400/0x420/0x440,
               ADC0 0x600, ADC1 0x640, AC0..2 0x680.., DAC0..2 0x6A0..,
               USART0 0x800, TWI0 0x810, SPI0 0x820,
               TCA0 0xA00, TCB0/1 0xA40/0xA50, TCD0 0xA80, SYSCFG 0xF00
0x1000–0x13FF  NVMCTRL regs, SIGROW, FUSE, USERROW
0x1400–0x14FF  EEPROM (mapped)
0x3800–0x3FFF  SRAM
0x8000–0xFFFF  Flash (mapped, for unified LD/`__flash` access)
```

---

## 3. Gap analysis — why the engine needs changes

These are the concrete blockers found in the current source.

1. **I/O callback table is far too small and wrongly addressed.**
   `MAX_IOs = 280` (`sim_avr.h:86`). The `avr->io[]` callback array is indexed
   by `AVR_DATA_TO_IO(addr) = addr-32`. Modern peripherals span data
   `0x0040–0x1FFF`, i.e. I/O indices up to ~8160. The table cannot reach them.
   The header even flags this: *"If you wanted to emulate … XMegas, this would
   need work."*

2. **The classic IO/data 0x20 offset is hardwired into the decoder.**
   `IN/OUT/SBI/CBI/SBIC/SBIS` add `+32` to the I/O operand to get the data
   address (`sim_core.c:542,575`; macros in `sim_core_declare.h:32-36`). On
   modern AVR the low-I/O space (`IN/OUT`, addresses `0x00–0x3F`) maps to data
   `0x00–0x3F` with **no** `+0x20` offset. VPORTs and `CCP`/`SP`/`SREG` are
   reached this way.

3. **SP and SREG positions are compile-time constants for the classic map.**
   `R_SPL = 32+0x3d (=0x5D)`, `R_SREG = 32+0x3f (=0x5F)` (`sim_avr.h:80-83`),
   used directly in `sim_core.c` (lines 297, 346, 351, 370…). On modern AVR
   SP=`0x3D/0x3E`, SREG=`0x3F`. The stack/SREG accessors must become
   per-core, not enum constants.

4. **Reset/IO vector & init plumbing assumes classic SFRs.**
   `sim_core_declare.h` `DEFAULT_CORE` references `MCUSR`, `LFUSE/HFUSE/EFUSE`,
   `RAMSTART`, etc. Modern AVR has none of these (reset flags live in
   `RSTCTRL.RSTFR`; 10 fuse bytes). A modern `DEFAULT_CORE` variant is needed.

5. **No Configuration Change Protection (CCP) model.** Writes to protected
   registers (CLKCTRL, WDT, many others) require writing a signature to `CCP`
   first, granting a 4-instruction unlock window. Nothing in the engine
   models this.

6. **Interrupt controller model mismatch.** simavr's `sim_interrupts.c` is built
   around per-peripheral enable/raise reg-bits and a flat priority. CPUINT adds:
   a single vector table base (`CPUINT.STATUS`/`LVL0/LVL1`/NMI), round-robin
   scheduling, and **interrupt-flag-in-peripheral** semantics (e.g. `INTFLAGS`
   registers, flag cleared by writing 1). The existing reg-bit interrupt
   primitive mostly fits, but vector dispatch and NMI need a modern path.

7. **Instruction timing differs (AVRxt vs AVRe+).** Several instructions have
   different cycle counts on AVRxt: `PUSH` 1 (was 2), `CBI/SBI` 1 (was 2),
   `LDS` 3, `ST`→2, `CALL`/`RCALL` one cycle less, etc. The decoder hardcodes
   classic cycle counts.

8. **Vector size.** With 16 K-word flash the toolchain emits 4-byte (`JMP`)
   vectors. `vector_size` must be confirmed and set accordingly (likely 4).

---

## 4. Architectural decision

Introduce an explicit **"modern AVR" mode** in the engine rather than trying to
make one code path serve both. Keep the classic path byte-for-byte unchanged to
avoid regressions across the dozens of existing cores.

Mechanism: add a small descriptor to `avr_t`, e.g.

```c
struct {
    uint8_t  io_offset;     // 0x20 classic, 0x00 modern (IN/OUT/SBI/CBI base)
    uint16_t sp_addr;       // data addr of SPL (0x5D classic, 0x3D modern)
    uint16_t sreg_addr;     // data addr of SREG (0x5F classic, 0x3F modern)
    uint8_t  timing;        // AVR_TIMING_CLASSIC | AVR_TIMING_XT
    uint8_t  has_ccp;       // enable CCP unlock model
} arch;
```

Replace the `R_SPL`/`R_SREG`/`+32` literals in `sim_core.c` with these fields.
For the classic cores the values are initialised to the current constants, so
behaviour is identical. This keeps the change surgical and testable.

---

## 5. Work breakdown (phased)

### Phase 0 — Plumbing & build (low risk)
- Add `arch` fields to `avr_t`; default-init to classic values in
  `avr_core_allocate`/`avr_init`.
- Confirm the core auto-discovery (Makefile greps `cores/*.c` for `avr_kind_t`,
  builds `sim_core_decl.h`) and the `CONFIG_*` gating pick up the new file with
  no Makefile edits — it should, by convention.
- Add a guarded include path for the ATtiny_DFP headers (they ship in
  `~/.mchp_packs/Microchip/ATtiny_DFP/.../include` and avr-gcc 16 already has
  `iotn3217.h`).

### Phase 1 — Engine: addressing model (the hard core change)
- Enlarge / restructure the I/O callback dispatch so addresses up to `0x1FFF`
  can carry read/write callbacks. Two options:
  - **(a) Grow `MAX_IOs`** to cover `0x2000` data bytes (~8 KB of callback
    structs). Simple, memory cost ~hundreds of KB per core — acceptable.
  - **(b) Sparse/segment dispatch** for the modern map only. More code, less
    memory. *Recommendation: start with (a)* behind the modern flag; optimise
    later if needed.
- Parameterise the `+32` IO offset and `R_SPL/R_SREG` accesses in `sim_core.c`
  using the `arch` fields.
- Make `data[]` allocation cover the full modern data span up to `RAMEND`
  (already driven by `ramend`; verify flash-mapping region `0x8000+` is handled
  for `LD`/`LPM`-style reads — modern code uses `LD` from mapped flash).

### Phase 2 — Engine: AVRxt instruction timing & CCP
- Add an `AVR_TIMING_XT` branch to the cycle accounting in `sim_core.c` for the
  handful of instructions that differ. (Functional behaviour is unchanged; only
  `cycle++` counts differ.)
- Implement CCP: a tiny state machine watching writes to `0x0034`; on the right
  signature, open an N-instruction window during which protected registers
  accept writes. Model as a write-callback on the CCP address plus a
  countdown decremented per instruction.

### Phase 3 — Engine: CPUINT interrupt controller
- Add a modern interrupt-dispatch path: single vector table, LVL0/LVL1/NMI,
  round-robin "last acknowledged vector" tracking.
- Keep using the existing `avr_int_vector` reg-bit primitive for
  enable/raised bits, but point "raised" at peripheral `INTFLAGS` bits and
  honour write-1-to-clear semantics.

### Phase 4 — Peripherals (incremental, prioritised)
Implement as new `avr_*` modules under `simavr/sim/` mirroring the existing
pattern (`avr_io_t` self-registration via `avr_register_io_write/read`). Modern
peripherals are register-block based, so each module takes a base address.

Priority order (most firmware needs the first tier):

1. **CLKCTRL** — needed so `frequency` is right (20/16 MHz osc + prescaler).
   Mostly a register model that recomputes `avr->frequency`.
2. **PORT + VPORT + PORTMUX** — `avr_ioport.c` analogue. VPORT is a
   bit-addressable alias of PORT `OUT/IN/DIR`; pin-change/port ISR via
   `PORTx.INTFLAGS`. Reuse the existing IOPORT IRQ/pin abstraction.
3. **CPUINT-driven SysTick sources: TCB** (simple), then **TCA0**
   (16-bit, split mode, WGM modes, compare outputs). Model on `avr_timer.c`.
4. **USART0** — modern register set (`CTRLA/B/C`, `STATUS`, `RXDATAL/H`,
   `TXDATAL`, `BAUD`). Reuse `avr_uart.c` IRQ/fifo plumbing; new register glue.
5. **RTC + PIT** — periodic interrupt, common as a tick source.
6. **NVMCTRL** — EEPROM + flash self-program via command register (replaces
   classic `SPMCSR`/`EECR`). Reuse `avr_eeprom.c`/`avr_flash.c` back-ends.
7. **SPI0**, **TWI0** — modern register glue over existing `avr_spi.c`/
   `avr_twi.c` back-ends.
8. **ADC0** — new 10/12-bit modern ADC. Reuse `avr_adc.c` IRQ model.
9. Lower priority / stubs first: **AC, DAC, CCL, EVSYS, CRCSCAN, VREF, BOD,
   SLPCTRL, RSTCTRL, WDT(new), TCD0**. Provide register-storage stubs so
   firmware that merely writes config doesn't crash; flesh out on demand.

### Phase 5 — The core descriptor
Create `simavr/cores/sim_tiny3217.c` (+ optional shared
`sim_tinyxy7.h` template for the 3216/3217/1617 family), following the
`sim_tiny85.c` pattern:
```c
#define SIM_MMCU      "attiny3217"
#define SIM_CORENAME  mcu_tiny3217
#define SIM_VECTOR_SIZE 4            // confirm via toolchain
#include "avr/iotn3217.h"
#include "sim_tinyxy7.h"            // declares struct mcu_t + peripheral wiring
avr_kind_t tiny3217 = { .names = { "attiny3217" }, .make = make };
```
A **modern `DEFAULT_CORE` variant** in a new `sim_core_declare_modern.h` fills
`ramend/flashend/e2end/signature`, the 10-byte fuse array, modern reset flags
(`RSTCTRL.RSTFR`), and the `arch` block (io_offset=0, sp=0x3D, sreg=0x3F,
timing=XT, has_ccp=1).

---

## 6. Testing strategy

- **Unit firmware** under `tests/` built with the modern toolchain
  (`-mmcu=attiny3217`), one per peripheral: blink (PORT), TCB/TCA periodic
  interrupt count, USART loopback, EEPROM write/read, RTC tick.
- Reuse simavr's existing test harness (`tests/` + `run_avr`); assert on
  GPIO IRQ traces / VCD output and on cycle counts for timing checks.
- **Cross-check cycle timing** of a known loop against the AVRxt datasheet
  numbers to validate the timing branch.
- Add a CI matrix entry only once the toolchain is reliably available
  (the DFP path is non-standard, so gate the modern tests behind a
  `HAVE_MODERN_AVR_TOOLCHAIN` make probe).

---

## 7. Risks & open questions

- **Memory cost of growing `MAX_IOs`** to ~0x2000 entries per core (struct is
  several pointers). Acceptable for one instance; revisit if many cores are
  instantiated. The sparse-dispatch fallback (option b) mitigates.
- **Vector size (2 vs 4 bytes)** for 16 KW flash — verify against the linked
  ELF (`avr-objdump -d` of a built vector table) before fixing `SIM_VECTOR_SIZE`.
- **Flash-in-data-space reads** (`0x8000+`): confirm `_avr_get_ram`/`LD`
  decode path resolves mapped flash; modern code reads `const __flash` via `LD`.
- **CCP window length / exact protected-register set** — follow the datasheet
  "Sequence for write operation to configuration change protected I/O
  registers".
- **CPUINT round-robin & NMI** corner cases (CRCSCAN NMI is vector 1) — model
  may need iteration to match priority behaviour.
- Scope creep across ~15 new peripherals — mitigate by shipping **stubs first**
  (register storage, no behaviour) so firmware boots, then deepening the
  high-value peripherals.

---

## 8. Suggested milestones

1. **M1 – Boots & blinks:** engine addressing/SP/SREG/timing + CCP + CLKCTRL +
   PORT/VPORT + CPUINT. A blink firmware runs and toggles a pin (verifiable via
   IRQ/VCD).
2. **M2 – Timers & serial:** TCB/TCA + USART0 + RTC. Periodic-interrupt and
   UART-loopback tests pass.
3. **M3 – Memory & buses:** NVMCTRL (EEPROM/selfprog) + SPI0 + TWI0 + ADC0.
4. **M4 – Breadth & polish:** remaining peripherals beyond stubs, cycle-timing
   validation, CI integration, docs.

---

## Implementation progress

### Phase 1 — addressing model — **DONE**
Architecture-variant abstraction (`avr->arch`: `flags`, `io_offset`, `sp_addr`,
`sreg_addr`); IO callback table converted to a per-core dynamically-sized
allocation (`io` pointer + `io_count`). Decoder, gdb stub and IO registration
parameterised. Classic cores unchanged (regression-tested). See audit notes.

### Phase 2 — AVRxt timing + CCP + flash-in-data-space — **DONE**
- **AVRxt instruction timing** (gated by `AVR_ARCH_F_XT_TIMING`), per the AVR
  Instruction Set Manual DS40002198 AVRe-vs-AVRxt cycle tables:
  ST/STD −1 (→1), PUSH −1 (→1), SBI/CBI −1 (→1), CALL/RCALL/ICALL −1,
  LDS +1 (→3). LD/LDD/STS/POP unchanged. `AVR_XT(avr)` macro in `sim_core.c`.
- **CCP** (`AVR_ARCH_F_CCP`): `arch.ccp_addr`/`arch.ccp_window`,
  `avr_ccp_write()` / `avr_ccp_io_write_enabled()`, an internal CCP-register
  write hook auto-installed for modern cores, and a per-instruction window
  countdown in `avr_run_one`. Window = `AVR_CCP_WINDOW` (4) instructions.
- **Flash mapped into data space** (`arch.flashmap_start`): `_avr_get_ram`
  redirects reads at/above the map base to `flash[]`; `_avr_set_ram` ignores
  writes there (self-programming goes via NVMCTRL, Phase 4).
- Verified by `tests/test_avrxt_engine.c` (26 checks, integrated into the
  `make run_tests` harness): every timing delta, CCP open/countdown, and a
  mapped-flash LDS read.

**Phase 2 audit (facts checked against DS40002198 + DS40002205A):**
- Timing baselines verified via the instruction-manual footnotes: `(1)` =
  internal-RAM assumption (simavr is internal-only ⇒ base counts hold);
  `(2)` = AVRxt adds ≥1 cycle for NVM-mapped load/store. So all AVRxt base
  numbers used (ST 1, LDS 3, …) are correct for the SRAM case. Full
  instruction sweep confirmed no AVRxt-differing opcode was missed.
- **Bug found & fixed:** CCP window granted only 3 instructions; the datasheet
  (8.5.7.1) specifies the protected write must occur "within four instructions"
  *after* the CCP write. The end-of-instruction countdown also fired on the
  CCP-writing instruction itself. Fixed by arming to `AVR_CCP_WINDOW + 1`;
  unit test now asserts exactly 4.

Deliberate simplifications (documented, permissive — only affect detection of
*buggy* firmware, never correct firmware):
- CCP window is a plain countdown; it does not close early on the first I/O/data
  write, nor on NVM access / SLEEP (datasheet says it should). Harmless because
  correct firmware writes once, immediately.
- AVRxt `(2)` NVM `+1` cycle on mapped-flash/EEPROM access is not modelled.

**Phase 3 dependency surfaced by the audit:** per DS40002205A 8.5.7, *interrupts
are ignored for the duration of the CCP period* (requests stay pending). When
the CPUINT dispatch is added in Phase 3 it must suppress servicing while
`avr->arch.ccp_window > 0`.

### Phase 3 — CPUINT interrupt controller — **DONE**
A modern interrupt-dispatch path (gated by `AVR_ARCH_F_CPUINT`) added alongside
the untouched classic path. Verified against DS40002205A §13 and Microchip
AN1982 / developer docs:
- **I-bit semantics (the big one):** on modern CPUINT the I bit is *not* cleared
  on interrupt entry and **RETI does not set it**; nesting is governed by the
  execution-level flags in `CPUINT.STATUS` (LVL0EX/LVL1EX/NMIEX). The RETI
  opcode handler and dispatch were gated accordingly.
- **NMI** (`vector.nmi`, e.g. CRCSCAN): serviced regardless of I, top priority,
  cannot be preempted; sets NMIEX.
- **LVL1** (one vector via `CPUINT.LVL1VEC`): preempts a running LVL0 handler.
- **LVL0 scheduling:** static (default lowest-vector-first), modified-static via
  `LVL0PRI`, and round-robin via `LVL0RR` (acknowledged vector becomes lowest
  priority). Implemented as a priority-rank with `LVL0PRI` wrap.
- **Sticky flags:** modern peripherals' INTFLAGS are not auto-cleared on entry —
  reuses the existing per-vector `raise_sticky` (set by the core).
- **CCP suppression:** interrupts are not serviced while `arch.ccp_window > 0`
  (the Phase 2 audit requirement).
- New engine API for the (Phase 4/5) CPUINT register block:
  `avr_cpuint_set_lvl1vec/lvl0pri/lvl0rr()`, `avr_cpuint_get_status()`, plus
  `avr->interrupts.cpuint_*` state.
- Verified by `tests/test_avrxt_engine.c` (now 41 checks): LVL0 dispatch + RETI
  (I untouched, LVL0EX toggled), NMI with I=0, LVL1 preempting LVL0, LVL0 not
  preempting LVL0, static and modified-static priority, CCP suppression. Classic
  interrupt-driven firmware (UART/timer/pin-change/IRQ) regression-clean.

Deferred (refinements, not needed by typical firmware): IVSEL vector relocation
and CVT compact vector table (vectors assumed at flash 0); the SP-write
4-instruction interrupt-hold window (DS §8.5.4); the LVL0 wrap uses the full
vector-number range rather than exact IVEC adjacency (negligible). The CPUINT
register block at 0x110 itself is wired in Phase 4/5 via the API above.

### Phase 4 — peripheral: TWI0 (modern I²C) — **DONE (first peripheral)**
Modern TWI0 host + client register front-end (`avr_twi_modern.[ch]`), driving
the *same* wire IRQ protocol as the classic `avr_twi.c` (`TWI_IRQ_INPUT/_OUTPUT/
_STATUS` carrying `TWI_COND_*` messages). It is therefore bus-compatible with
the existing simavr I²C parts (`examples/parts/i2c_eeprom.c`, `ds1338`, …) and
with the classic TWI peripheral — a modern master can drive a classic slave and
vice-versa, and it self-registers under `AVR_IOCTL_TWI_GETIRQ(name)` so
`i2c_eeprom_attach()` works unchanged.

- **Register block** at a single base (0x810 on ATtiny3217), 16 bytes:
  CTRLA/DBGCTRL, host MCTRLA/MCTRLB/MSTATUS/MBAUD/MADDR/MDATA, client
  SCTRLA/SCTRLB/SSTATUS/SADDR/SDATA/SADDRMASK. Layout matches the device
  header `TWI_t`.
- **Host (master):** writing MADDR issues START+address and (for reads) clocks
  in the first byte; MDATA write/read transmits/receives; MCTRLB MCMD strobes
  drive REPSTART / RECVTRANS (ACK+next) / STOP. MSTATUS models BUSSTATE,
  RXACK (ACK/NACK), CLKHOLD, and the WIF/RIF flags + W1C clears. Two CPUINT
  vectors: **TWIM** (host, vec 25) gated by WIEN/RIEN, **TWIS** (client,
  vec 24) by APIEN/PIEN/DIEN; flags are sticky (software-cleared), matching the
  CPUINT model.
- **Client (slave):** address match (SADDR/SADDRMASK) raises APIF + AP/DIR and
  holds the clock; data in/out via SDATA with DIF; STOP raises APIF. SCTRLB
  SCMD releases the held flags.
- **Engine fix surfaced by this work:** `avr_regbit_t.reg` was only **9 bits**
  (max 0x1FF), so it could not address modern peripheral registers (every
  interrupt-enable bit lives at 0x800+). Widened to **13 bits** (covers the
  0x1FFF modern I/O span; 13+3+8 = 24 bits still pack into the uint32 and are
  still register-passed). This was the missing piece of the Phase-1 addressing
  model — without it no modern peripheral interrupt can be enabled. Classic
  cores are unaffected (all existing `.tst` regressions pass).
- Verified by `tests/test_avrxt_engine.c` (now 60 checks): master write/read
  transactions against a register-pointer slave over the wire (incl. address
  NACK to an absent slave, multi-byte read with ACK+continue), and the client
  path (address-match APIF + TWIS pending, data-in DIF/SDATA, STOP APIF).

Deliberate simplifications (documented; match the classic TWI's synchronous wire
model): the bus is delivered synchronously (a slave's reply lands during the
master's raise), so MBAUD/bit-timing is not cycle-modelled, and the client
auto-ACKs at the wire level (it cannot clock-stretch then NACK an address — the
synchronous bus has no place to defer). Smart-mode (SMEN), quick-command,
timeout/bus-error detection and dual-mode are not modelled.

### Phase 5 — core descriptor: `sim_tiny3217` — **DONE (TWI0 wired)**
The first modern core descriptor, `simavr/cores/sim_tiny3217.c`, following the
classic `sim_tiny85.c` pattern but using the new modern scaffolding:
- **`sim_core_declare_modern.h`** — `MODERN_CORE(_vector_size)` analogue of the
  classic `DEFAULT_CORE`. Fills `ioend` (0x1FFF, top of extended I/O), `ramend`/
  `flashend`/`e2end`/`signature` from the DFP header, and the whole `arch`
  block: `flags = MODERN|CCP|XT_TIMING|CPUINT`, `io_offset=0`, `sp_addr=0x3D`,
  `sreg_addr=0x3F`, `ccp_addr=0x34`, `flashmap_start=MAPPED_PROGMEM_START`
  (0x8000).
- **`cores/avr/iotn3217.h`** — the device header is bundled alongside the other
  `cores/avr/*.h` so the Makefile's core auto-discovery (host-`cc -E`) resolves
  it without the DFP on the include path. Confirmed `SIM_VECTOR_SIZE = 4` (the
  toolchain emits `JMP` vectors) via `avr-objdump` of a linked vector table.
- The core's `init` calls `avr_twi_modern_init(avr, &mcu->twi, 0x0810,
  TWI0_TWIM_vect_num, TWI0_TWIS_vect_num, '0')`. Auto-discovery picks the file
  up (`CONFIG_TINY3217`, `extern avr_kind_t tiny3217`) with no Makefile edits.
- **Engine fix:** `avr_t.fuse[6]` → `fuse[10]` (ATtiny3217 has
  `FUSE_MEMORY_SIZE = 10`); only affects ELF `.fuse` loading, classic cores
  unchanged.

Verified two ways in `tests/test_avrxt_engine.c` (now 75 checks):
1. `avr_make_mcu_by_name("attiny3217")` + `avr_init` reports the modern arch
   (MODERN/CPUINT, SP/SREG/CCP/flashmap, vector_size 4, RAMEND, signature) and
   the TWI0 MADDR/MCTRLB write hooks are registered at 0x810+; a master write
   transaction drives a loopback slave.
2. **Full end-to-end:** a bare-metal ATtiny3217 firmware built with avr-gcc
   (`TWI0.MADDR/MDATA/MCTRLB` via `iotn3217.h`) is loaded onto the core, run,
   and writes a byte into a real `examples/parts/i2c_eeprom.c` over the wire —
   exercising AVRxt execution, modern addressing (`STS 0x0816`, …), CPUINT and
   the TWI0 peripheral together.

### Phase 4 — peripheral: PORT / VPORT — **DONE**
Modern GPIO (`avr_port_modern.[ch]`), reusing the classic IOPORT IRQ/ioctl
abstraction (`avr_ioport.h`) so existing simavr parts (LEDs, buttons) and VCD
wiring connect unchanged. Wired into `sim_tiny3217` as PORTA/B/C (0x400/0x420/
0x440) with VPORTA/B/C (0x00/0x04/0x08).
- **Full PORT block:** DIR/OUT/IN, the SET/CLR/TGL write-strobe aliases (which
  read back DIR/OUT), per-pin PINnCTRL (PULLUPEN internal pull-ups, INVEN
  inversion, ISC sense), INTFLAGS, and "write 1s to IN toggles OUT".
- **Pin interrupts:** per-pin PINnCTRL.ISC (BOTHEDGES/RISING/FALLING/LEVEL);
  a triggered input sets its INTFLAGS bit and raises `PORTx_PORT`. The vector's
  "enable" is INTFLAGS itself (mask 0xFF) so it queues while any flag is set and
  is W1C-cleared — same sticky pattern as TWI.
- **VPORT (the hard part):** modern VPORTs are bit-addressable aliases in the
  low I/O space (0x00..0x0B) so firmware uses single-cycle `SBI`/`CBI`/`OUT` on
  them. But simavr keeps the GP register file r0..r31 in `data[0..31]`, which
  *overlaps* the VPORT addresses. The fix is a new engine **low-I/O redirect**:
  `avr->lowio_redirect[0x40]` is consulted only on the *memory-access* path
  (`_avr_set_ram`/`_avr_get_ram`), so `SBI VPORTA_OUT` resolves to `PORTA.OUT`
  at 0x404, while *register operands* (which never go through those functions)
  keep using `data[]`. Zero-cost for classic cores (table is empty;
  short-circuited). `avr_port_modern_init()` installs the four redirects per
  VPORT. (A sentinel `AVR_PORT_MODERN_NO_VPORT = 0xFFFF` distinguishes "no
  VPORT" from the legitimate VPORTA base of 0x0000.)

Verified in `tests/test_avrxt_engine.c` (now 94 checks): register access and the
SET/CLR/TGL aliases; pin output observed via the PIN_ALL IRQ; pin-change
interrupt via ISC + INTFLAGS W1C; and **executed** `SBI`/`CBI`/`STS` to VPORTA
landing on PORTA with **r1 left untouched**. End-to-end: an avr-gcc firmware
that drives PORTA via `OUT`/`SBI`/`CBI` on VPORT and `STS` on PORTA.OUTSET
produces the expected pin levels (0xD6) on the core.

### Phase 4 — peripheral: CLKCTRL — **DONE**
Main-clock controller (`avr_clkctrl.[ch]`), wired into `sim_tiny3217` at 0x60.
Its job is to keep `avr->frequency` in step with what the firmware configures so
cycle-time conversions and timers are correct.
- **MCLKCTRLA.CLKSEL** selects the source: OSC20M (16/20 MHz, chosen from
  `FUSE.OSCCFG.FREQSEL`), OSCULP32K / XOSC32K (32.768 kHz), or EXTCLK.
- **MCLKCTRLB.PEN/PDIV** applies the prescaler (the full 2/4/8/16/32/64/6/10/12/
  24/48 division table); `avr->frequency = base / div` is recomputed on every
  change.
- **CCP-gated:** MCLKCTRLA/B and MCLKLOCK are Configuration-Change-Protected —
  writes are honoured only while `avr_ccp_io_write_enabled()` (so real
  `_PROTECTED_WRITE`/`ccp_write_io` sequences work, and a bare write is ignored).
  **MCLKLOCK.LOCKEN** makes the clock config read-only until reset.
- **Reset default:** OSC20M with PDIV = 6X (MCLKCTRLB = 0x11) → the documented
  ~3.33 MHz CLK_PER on a 20 MHz part; `MODERN_CORE` seeds `.frequency` to match.
- MCLKSTATUS reflects the selected source as "stable"; it is read-only.

Verified in `tests/test_avrxt_engine.c` (now 101 checks): reset default
3.33 MHz, PEN/PDIV → 20/5/3.33 MHz, source switch to 32.768 kHz, a bare
(non-CCP) write ignored, and LOCKEN freezing the config. End-to-end: an avr-gcc
firmware using `_PROTECTED_WRITE(CLKCTRL.MCLKCTRLB, PEN|PDIV_4X)` takes the core
from 3.33 MHz to 5 MHz at run time (exercising the CCP window + CLKCTRL).

### Phase 4 — peripheral: TCB (16-bit Timer type B) — **DONE**
`avr_tcb.[ch]`, wired into `sim_tiny3217` as TCB0/TCB1 (0xA40/0xA50, vectors
13/14). Scope is the dominant "periodic tick" use — **Periodic Interrupt mode**
(CNTMODE = INT/TIMEOUT):
- The counter is driven by a simavr **cycle timer** scheduled `(CCMP+1)*prescale`
  CPU cycles ahead. On each expiry the **CAPT** flag (INTFLAGS bit0) is set and
  `TCBn_INT` is raised (if INTCTRL.CAPT is enabled), then it reschedules — a
  clean periodic source with no per-cycle overhead.
- **CTRLA.CLKSEL:** CLKDIV1 (CLK_PER) and CLKDIV2 (CLK_PER/2); CLKTCA falls back
  to CLK_PER (TCA prescaler not yet modelled). CTRLA.ENABLE starts/stops and
  drives STATUS.RUN.
- **CNT** reads return a computed live value (elapsed cycles → ticks, with the
  high byte latched on low-byte read, as the hardware does via TEMP).
- INTFLAGS is W1C; the CAPT vector uses the standard engine raised/enable
  reg-bits with `raise_sticky` (software-cleared, matching modern INTFLAGS).
- Other count modes (input capture / single-shot / 8-bit PWM) and event inputs
  are not modelled; their registers still store so configuring firmware is fine.

Verified in `tests/test_avrxt_engine.c` (now 110 checks): STATUS.RUN, first CAPT
at ~101 cycles for CCMP=100, the periodic cadence after W1C, live CNT read,
clean stop on disable, and CLKDIV2 doubling the period. End-to-end: an avr-gcc
firmware with `ISR(TCB0_INT_vect)` toggling PA0 every 501 cycles produces exactly
99 toggles in 50 000 cycles — exercising TCB + CPUINT dispatch + PORT together.

### Phase 4 — peripheral: TCA0 (16-bit Timer type A) — **DONE**
`avr_tca.[ch]`, wired into `sim_tiny3217` at 0xA00 (vectors OVF=8, CMP0/1/2 =
10/11/12). Models **single (16-bit) mode**: a prescaled up-counter (0..TOP,
TOP = PER) with the overflow and three compare-match interrupts.
- **Next-event scheduler:** instead of stepping every cycle, one simavr cycle
  timer is scheduled to the nearest interesting count — the smallest compare
  value above the current count, or the wrap (TOP+1). On expiry it sets the
  matching INTFLAGS (CMP0/1/2 and/or OVF), raises those vectors, and schedules
  the next event. Cheap and exact for the interrupt timing.
- **CTRLA.CLKSEL** prescaler: 1/2/4/8/16/64/256/1024. ENABLE starts/stops.
- **PER** is TOP; **CMP0/1/2** are the compare values (read live from the
  registers, so changes take effect at the next event). **CNT** reads compute
  the live count (high byte latched); writing CNT re-anchors the phase.
- Four interrupt vectors use the standard engine raised/enable reg-bits
  (INTCTRL/INTFLAGS bits OVF=0, CMP0=4, CMP1=5, CMP2=6) with `raise_sticky`;
  INTFLAGS is W1C.
- Split (dual 8-bit) mode and the waveform-output pins (WO0..5 via PORTMUX) are
  not modelled yet; their registers still store so configuring firmware is fine.

Verified in `tests/test_avrxt_engine.c` (now 118 checks): CMP1 match at CNT=50,
overflow at TOP+1 = 201 cycles, pending interrupt, live CNT read, W1C, overflow
cadence, the /4 prescaler stretching the period to ~804 cycles, and a clean stop
on disable. End-to-end: an avr-gcc firmware with `ISR(TCA0_OVF_vect)` and
`CLKSEL_DIV4 | PER=999` toggles PA0 every 4000 cycles (~20 toggles in 80 000
cycles) — TCA0 + prescaler + CPUINT + PORT together.

### Phase 4 — peripheral: USART0 — **DONE**
`avr_usart_modern.[ch]`, wired into `sim_tiny3217` at 0x800 (vectors RXC=27,
DRE=28, TXC=29). It reuses the **classic UART wire IRQ convention**
(`UART_IRQ_INPUT`/`_OUTPUT`, `AVR_IOCTL_UART_GETIRQ`) so the existing simavr
UART endpoints (uart_pty, uart_udp, the simduino bridge) connect unchanged; only
the register glue is new.
- **Transmit:** writing TXDATAL (with CTRLB.TXEN) emits the byte on
  `UART_IRQ_OUTPUT` immediately and arms a cycle timer for the frame time after
  which TXCIF asserts (TXC interrupt if enabled). The data register reads as
  always empty (DREIF stays set), which drives both polled and DRE-interrupt
  transmitters.
- **Receive:** `UART_IRQ_INPUT` (with CTRLB.RXEN) queues the byte in a 256-entry
  fifo and sets RXCIF (RXC interrupt if enabled); reading RXDATAL pops it and
  clears RXCIF when the fifo drains. RXDATAH bit7 mirrors RXCIF.
- **Frame time** is derived from BAUD (async-normal: CLK_PER cycles/bit = BAUD/4,
  ~10-bit frame), so TXC ordering/timing is reasonable. STATUS.TXCIF is W1C;
  CTRLA enables (RXCIE/DREIE/TXCIE) re-raise already-pending flags. 9-bit/parity/
  sync/one-wire modes are not modelled (registers still store).

Verified in `tests/test_avrxt_engine.c` (now 133 checks): reset DREIF, TX byte on
the OUTPUT IRQ, deferred TXCIF + W1C, RXCIF on input, RXDATAL read, in-order fifo
draining, RXC pending when enabled, and input ignored with RXEN=0. End-to-end: an
avr-gcc echo firmware (`RXCIF`→`RXDATAL`→poll `DREIF`→`TXDATAL`) round-trips
"Hi!" through the core's USART0 wire IRQs.

### Phase 4 — peripheral: NVMCTRL (EEPROM) — **DONE**
`avr_nvmctrl.[ch]`, wired into `sim_tiny3217` at 0x1000 (EE-ready vector = 30),
managing the memory-mapped EEPROM at 0x1400 (256 B). It replaces the classic
EECR/SPMCSR with the modern command-register model.
- **EEPROM storage** lives in `avr->data[0x1400..]`, so it is read back with an
  ordinary load from the mapped address. Writes to that region do **not** land
  there directly — they accumulate in a **page buffer** with a per-byte dirty
  mask, mirroring how the hardware loads the buffer before a command.
- **CTRLA.CMD** (CCP-protected — honoured only inside a CCP window, so real
  `_PROTECTED_WRITE_SPM` sequences work): PAGEWRITE / PAGEERASEWRITE commit the
  dirty buffer bytes; PAGEERASE sets them to 0xFF; PAGEBUFCLR discards the
  buffer; EEERASE / CHIPERASE wipe the whole EEPROM. Completion sets
  INTFLAGS.EEREADY and raises `NVMCTRL_EE` if enabled.
- Commands complete instantly (STATUS.EEBUSY/FBUSY never observed set), so the
  usual `while (NVMCTRL.STATUS & EEBUSY)` poll passes. INTFLAGS is W1C.
- The committed-bytes-only flush means single-byte writes preserve their
  neighbours (matching practical use). Flash self-programming is not modelled
  (writes to mapped flash are ignored by the engine).

Verified in `tests/test_avrxt_engine.c` (now 147 checks): erased read 0xFF,
buffered write not visible until the command, commit ignored without CCP,
ERASEWRITE/PAGEWRITE commit (neighbours preserved), PAGEBUFCLR abort, the
EE-ready interrupt + W1C, and EEERASE wiping the array. End-to-end: an avr-gcc
firmware writes two EEPROM bytes via `_PROTECTED_WRITE_SPM(NVMCTRL.CTRLA,
PAGEERASEWRITE)`, reads them back, and drives their XOR (0x99) onto PORTA —
EEPROM + CCP + PORT together.

### Phase 4 — peripheral: RTC + PIT — **DONE**
`avr_rtc.[ch]`, wired into `sim_tiny3217` at 0x140 (vectors RTC_CNT=6, RTC_PIT=7).
The block hosts two independent functions sharing one clock source. Because the
RTC clock (~32 kHz) is **decoupled from CLK_PER**, periods are converted to CPU
cycles via `avr->frequency` captured when each function (re)starts —
`cpu_cycles = N * avr->frequency / f_rtc`.
- **Clock source (CLKSEL):** INT32K / TOSC32K / EXTCLK modelled as 32.768 kHz,
  INT1K as 1.024 kHz.
- **RTC counter:** a prescaled (DIV1..DIV32768) up-counter 0..PER. Uses the same
  next-event scheduler as TCA0 — one cycle timer to the next interesting count
  (the compare value, or the PER+1 wrap). On expiry it sets INTFLAGS.CMP and/or
  .OVF and raises the **single RTC_CNT vector** if that source is enabled. CNT
  reads compute the live value (high byte latched). PER resets to 0xFFFF.
- **Shared vector (the wrinkle):** OVF and CMP both feed RTC_CNT. As with the
  modern TWI's TWIM/TWIS, `.raised` is left unset and the flags are set/cleared
  directly; `.enable` spans both INTCTRL bits so the engine sees "either", and
  the precise flag/enable pairing is enforced in `rtc_cnt_flag()`. Enabling a
  source whose flag is already set raises immediately (INTCTRL write hook).
- **PIT:** a free-running periodic source firing every 2^n RTC-clock ticks
  (PITCTRLA.PERIOD = CYC4..CYC32768), on its own RTC_PIT vector, **independent of
  the RTC prescaler**. Single PI flag → single vector (standard raised/enable
  bits + `raise_sticky`). INTFLAGS / PITINTFLAGS are W1C.

Deliberate simplifications: the synchronisation-busy STATUS/PITSTATUS bits are
never asserted (writes take effect immediately, so the usual busy poll passes); a
CLK_PER change after the RTC starts is not retro-applied until the function is
reconfigured; CRYSTERR / external-clock pin behaviour is not modelled.

Verified in `tests/test_avrxt_engine.c` (now 162 checks): PER reset 0xFFFF, CMP
match near 202 cycles + enabled-interrupt raise, live CNT read, CMP W1C, OVF at
the PER+1 wrap (~505 cycles), clean stop on disable; the RTC_CNT gating (OVF flag
sets while masked with nothing raised, then enabling the set flag raises now);
and the PIT (CYC4 → first PI ~406 cycles, enabled-interrupt raise, W1C, periodic
cadence, clean stop on disable).

### Phase 4 — peripheral: ADC0 — **DONE**
`avr_adc_modern.[ch]`, wired into `sim_tiny3217` at 0x600 (vectors RESRDY=20,
WCOMP=21). Models single-shot and free-running conversions; reuses the classic
ADC's "analog input as a wire IRQ" idea — each channel's voltage (millivolts) is
presented by raising the matching AINn IRQ (`AVR_IOCTL_ADCM_GETIRQ(name)`).
- **Conversion:** writing COMMAND.STCONV (with CTRLA.ENABLE) schedules a simavr
  cycle timer for a realistic duration (~13 ADC clocks at the CTRLC.PRESC
  prescaler, DIV2..DIV256 → 26..3328 CPU cycles). On completion the MUXPOS
  channel's mV is converted against `vref_mv` into RES at 10-bit
  (CTRLA.RESSEL=0) or 8-bit resolution, STCONV self-clears, INTFLAGS.RESRDY is
  set and ADC0_RESRDY raised if enabled.
- **Free-running (CTRLA.FREERUN):** the completion handler re-queues the next
  conversion, giving a steady RESRDY cadence; clearing ENABLE stops the stream.
- **Window comparator (CTRLE.WINCM):** BELOW / ABOVE / INSIDE / OUTSIDE evaluated
  against WINLT/WINHT after each conversion; a hit sets INTFLAGS.WCMP and raises
  ADC0_WCOMP if enabled. Both flags are W1C; both vectors use `raise_sticky`.
- **Reference:** `vref_mv` defaults to 3300 mV and is settable via
  `avr_adc_modern_set_vref()` (the VREF peripheral / CTRLC.REFSEL is not
  modelled).

Deliberate simplifications: sample accumulation (CTRLB.SAMPNUM) is treated as a
single sample; exact reference selection, event-triggered start, and the
temperature-sensor / DAC / internal channels are not modelled (registers still
store, so configuring firmware is fine).

Verified in `tests/test_avrxt_engine.c` (now 175 checks): AIN IRQ wiring, 10-bit
result 512 and 8-bit result 128 from 1650 mV against the 3300 mV vref, RESRDY
timing (~26 cycles at DIV2) + enabled-interrupt raise, STCONV self-clear, W1C,
free-running repeated conversions at the right cadence, clean stop on disable,
and the window comparator (ABOVE fires, INSIDE-of-a-non-matching-window does not)
with its WCOMP interrupt.

### Phase 4 — peripheral: SPI0 — **DONE**
`avr_spi_modern.[ch]`, wired into `sim_tiny3217` at 0x820 (vector SPI0_INT=26).
Models the normal (non-buffered) mode in host and client roles, driving the
*same* wire IRQ convention as the classic `avr_spi.c` (`SPI_IRQ_INPUT`/`_OUTPUT`,
`AVR_IOCTL_SPI_GETIRQ`) so existing simavr SPI endpoints connect unchanged — only
the register glue is new.
- **Host (master):** writing DATA schedules a transfer of `prescaler * 8` CPU
  cycles (CTRLA.PRESC DIV4/16/64/128, halved by CLK2X). On completion the byte is
  emitted on `SPI_IRQ_OUTPUT` (MOSI), INTFLAGS.IF is set and SPI0_INT raised if
  INTCTRL.IE. A connected part's synchronous reply on `SPI_IRQ_INPUT` (MISO) is
  latched into DATA as the received byte.
- **Client (slave):** a byte arriving on `SPI_IRQ_INPUT` is latched into DATA
  (setting IF) and the current DATA is echoed back on `SPI_IRQ_OUTPUT`.
- **Flags:** IF (INTFLAGS bit7) is enabled by INTCTRL.IE (bit0) and cleared by
  reading DATA (the normal-mode "read INTFLAGS then access DATA" sequence; a W1C
  is also accepted). Writing DATA mid-transfer sets WRCOL and is ignored.

Deliberate simplifications (consistent with the classic SPI's synchronous wire):
buffered mode (CTRLB.BUFEN and the RXCIF/TXCIF/DREIF/SSIF/BUFOVF flag set with
their separate enables), the SS client-select trigger, and exact CPOL/CPHA/bit
order are not modelled (the configuration still stores).

Verified in `tests/test_avrxt_engine.c` (now 187 checks): host transfer against a
complement-echo client — IF deferred then set at ~32 cycles (DIV4), the MOSI byte
observed, the MISO reply latched, the IE interrupt raised, IF cleared by the DATA
read, and a write-collision (mid-transfer DATA write sets WRCOL and is dropped,
the first byte still clocked); and the client path — a received byte latched with
IF set, the held DATA echoed on MISO, and IF cleared by the DATA read.

### Phase 4 — peripheral: AC0 (analog comparator) — **DONE**
`avr_ac.[ch]`, wired into `sim_tiny3217` at 0x680 (vector AC0_AC=17). Models the
comparator as a combinational function of its selected inputs: STATUS.STATE =
(V+ > V-), optionally inverted (MUXCTRLA.INVERT). Analog voltages (millivolts)
are presented on the AINP0..3 / AINN0..1 IRQs (`AVR_IOCTL_AC_GETIRQ(name)`).
- **Input MUX:** MUXCTRLA.MUXPOS picks AINP0..3; MUXNEG picks AINN0/AINN1, the
  internal VREF, or the DAC output. VREF/DAC are settable values
  (`avr_ac_set_refs()`, defaults 1100 mV / 0 mV) since those peripherals are not
  modelled.
- **Output:** recomputed on any input or config change; STATUS.STATE tracks the
  live output and it is mirrored on the OUT IRQ (so it can drive a pin or be
  observed). STATUS is refreshed on read too.
- **Interrupt:** the CTRLA.INTMODE edge (BOTHEDGE / POSEDGE / NEGEDGE) sets the
  W1C STATUS.CMP flag and raises AC0_AC if INTCTRL.CMP is enabled (`raise_sticky`,
  with enable-while-set re-raising).

Deliberate simplifications: hysteresis (CTRLA.HYSMODE), low-power / run-standby
timing and the physical OUTEN pin buffer are not modelled (the OUT IRQ is always
emitted; the configuration still stores).

Verified in `tests/test_avrxt_engine.c` (now 200 checks): STATE low while
disabled, STATE high/low tracking V+ vs V- with the OUT IRQ following, INVERT
flipping the output, a positive-edge interrupt (CMP flag + AC0_AC raise, W1C,
negedge ignored in POSEDGE mode), and comparison against the internal VREF.

### Phase 4 — peripheral: DAC0 (8-bit DAC) — **DONE**
`avr_dac.[ch]`, wired into `sim_tiny3217` at 0x6A0 (no interrupt). Models the
converted output voltage: when CTRLA.ENABLE is set, the output is
(DATA / 256) * VREF millivolts, otherwise 0. The value is published on the OUT
IRQ (`AVR_IOCTL_DAC_GETIRQ(name)`) whenever it changes.
- **Reference:** `vref_mv` defaults to 1100 mV and is settable via
  `avr_dac_set_vref()` (the VREF peripheral is not modelled).
- **On-chip routing:** the core wires DAC0's OUT IRQ to AC0's DAC negative input
  (`tiny3217_dac_to_ac` → `avr_ac_set_refs`), so an AC comparison with
  MUXNEG = DAC tracks the live DAC output — exactly as the silicon routes it.

Deliberate simplifications: the physical output pin buffer (CTRLA.OUTEN — the
OUT IRQ is always emitted), run-standby, and exact reference selection are not
modelled (the configuration still stores).

Verified in `tests/test_avrxt_engine.c` (now 207 checks): no output while
disabled, 550 mV at DATA=128 and 1095 mV at full-scale against the 1100 mV vref,
zero, output forced to 0 on disable; and the DAC0→AC0 routing (an AC0 comparison
of 800 mV against the DAC output flips as the DAC moves 550 → 1095 mV).

### Phase 4 — peripheral: CCL (configurable custom logic) — **DONE**
`avr_ccl.[ch]`, wired into `sim_tiny3217` at 0x1C0 with 2 LUTs (no interrupt).
Models the combinational core: each LUT computes a 3-input truth table (TRUTHn)
whose inputs are routed by the LUTnCTRLB/C.INSEL fields. The whole block is
re-evaluated on any config or IO-input change.
- **Input routing modelled:** MASK (constant 0), IO (an external level on the
  LUTn-INm IRQ), LINK (the next LUT's output) and FEEDBACK (the LUT's own
  output). Events and peripheral sources read as 0 (config still stores).
- **Evaluation:** because LINK/FEEDBACK route LUT outputs back as inputs, the
  evaluator iterates to a fixed point (bounded) before publishing; each LUT's
  output is published on its OUT IRQ when it changes. A LUT outputs 0 unless both
  CTRLA.ENABLE and its LUTnCTRLA.ENABLE are set.

Deliberate simplifications: the synchronizer/filter (FILTSEL), edge detector
(EDGEDET), clock source (CLKSRC) and the sequencer (SEQCTRL0) are not modelled —
those registers still store, so configuring firmware behaves; only the
timing/stateful behaviour is absent.

Verified in `tests/test_avrxt_engine.c` (now 215 checks): LUT0 wired as a 2-input
AND of IO pins (truth-table evaluation over the input combinations), a LINK chain
(LUT1 = NOT of LUT0's output, following it combinationally), and CTRLA.ENABLE
gating both outputs to 0 when cleared.

### Phase 4 — peripheral: EVSYS (event system) — **DONE**
`avr_evsys.[ch]`, wired into `sim_tiny3217` at 0x180 (no interrupt). Models the
event-routing fabric as an observable switch matrix:
- **Channels:** six — ch0/1 = SYNCCH0/1, ch2..5 = ASYNCCH0..3. Each carries a
  level (0/1) driven by a generator or test via the matching CHn IRQ, or pulsed
  by the ASYNCSTROBE/SYNCSTROBE registers.
- **Users:** each user register (ASYNCUSER0..12, SYNCUSER0..1) selects a channel
  — the select value v maps uniformly to channel v-1 (0 = off). When a routed
  channel changes (or a user is re-routed) the user's current value is emitted on
  its USERn OUT IRQ; one channel fans out to all users selecting it.

Deliberate simplification: the generator-selection registers (ASYNCCHn/SYNCCHn)
still store, but generators are not auto-wired into the fabric — a channel is
driven via its CHn IRQ (or the strobe). This keeps EVSYS observable and lets
event-aware peripherals/tests be connected later without engine changes.

Implementation note: `avr_io_setirqs()` builds each IRQ's name by dereferencing
`irq_names[i]`, so every entry must be non-NULL (a NULL crashes there, not in the
guarded `avr_init_irq` path) — EVSYS names all 21 IRQs.

Verified in `tests/test_avrxt_engine.c` (now 223 checks): a user following its
routed channel high/low, fan-out to two users on one channel, a re-routed user
receiving the channel's current level immediately, an "off" user receiving
nothing, and a software strobe pulsing the routed user once.

### Phase 4 — peripheral: PORTMUX — **DONE (config store)**
`avr_portmux.[ch]`, wired into `sim_tiny3217` at 0x200 (no interrupt). PORTMUX
selects which *physical pins* a peripheral's functions route to (alternate pin
sets for EVOUT/CCL-LUT, USART0, SPI0, TWI0, TCA0 WO0..5, TCB0/1 WO). simavr
connects peripherals through their function IRQs, not numbered physical pins, so
the pin selection has **no behavioural effect** here. The module is therefore a
registered configuration store: the four CTRL registers read back what firmware
writes and reset to 0 (default routing), so muxing code behaves. The single
write hook is where real pin re-routing could be added later if a peripheral
grows PORT-pin outputs.

Verified in `tests/test_avrxt_engine.c` (now 229 checks): CTRLA/CTRLB reset to 0,
CTRLA/B/C store and read back their selections, and a neighbouring register is
left untouched.

### Phase 4 — peripheral: TCD0 (12-bit timer type D) — **DONE**
`avr_tcd.[ch]`, wired into `sim_tiny3217` at 0x0A80 (vector OVF=15). TCD is an
asynchronous high-resolution-PWM timer; this models its periodic-overflow use.
- **Scheduling:** a simavr cycle timer is scheduled (CMPBCLR+1)*prescale CPU
  cycles ahead; on expiry the OVF flag is set and TCD0_OVF raised (if
  INTCTRL.OVF), then it reschedules — a clean periodic source. INTFLAGS is W1C.
- **Clock:** the TCD source is approximated as CLK_PER, divided by CTRLA.SYNCPRES
  (1/2/4/8) and CNTPRES (1/4/32). CMPBCLR (12-bit) is TOP.
- **Sync protocol:** STATUS always reads ENRDY|CMDRDY, so the double-buffered
  enable/command polling (`while (!(TCD0.STATUS & ENRDY))`) passes.

Deliberate simplifications: the waveform outputs (WOA/WOB), TRIGA/TRIGB compare
events, dithering, fault control and input capture are not modelled, and the
exact TCD clock source is approximated as CLK_PER (those registers still store).

Verified in `tests/test_avrxt_engine.c` (now 236 checks): STATUS ready, first OVF
at ~101 cycles for CMPBCLR=100 + enabled-interrupt raise, W1C, the periodic
cadence, clean stop on disable, and SYNCPRES=DIV2 doubling the period.

Still stubs/absent (firmware that only configures them will currently see plain
RAM at those addresses): WDT(new), CRCSCAN, SLPCTRL, RSTCTRL, and the rest —
added incrementally next.

## 9. Files touched (summary)

Engine: `sim_avr.h` (arch fields, MAX_IOs, `fuse[10]`, `lowio_redirect[]`),
`sim_avr.c` (init defaults), `sim_core.c` (offset/SP/SREG params, AVRxt timing,
low-I/O redirect in `_avr_set_ram`/`_avr_get_ram`), `sim_avr_types.h`
(`avr_regbit_t.reg` widened 9→13 bits for modern register addresses),
`sim_core_declare.h` (+ new `sim_core_declare_modern.h`),
`sim_interrupts.[ch]` (CPUINT path), new `avr_ccp.[ch]`.
Peripherals: **new `avr_twi_modern.[ch]` (TWI0 — DONE)**, **new
`avr_port_modern.[ch]` (PORT/VPORT — DONE)**, **new `avr_clkctrl.[ch]`
(CLKCTRL — DONE)**, **new `avr_tcb.[ch]` (TCB0/1 — DONE)**, **new
`avr_tca.[ch]` (TCA0 — DONE)**, **new `avr_usart_modern.[ch]` (USART0 —
DONE)**, **new `avr_nvmctrl.[ch]` (NVMCTRL/EEPROM — DONE)**, **new `avr_rtc.[ch]` (RTC + PIT — DONE)**, **new
`avr_adc_modern.[ch]` (ADC0 — DONE)**, **new
`avr_spi_modern.[ch]` (SPI0 — DONE)**, **new `avr_ac.[ch]` (AC0 — DONE)**,
**new `avr_dac.[ch]` (DAC0 — DONE)**, **new `avr_ccl.[ch]` (CCL — DONE)**, **new `avr_evsys.[ch]` (EVSYS — DONE)**, **new `avr_portmux.[ch]` (PORTMUX —
config store)**, **new `avr_tcd.[ch]` (TCD0 — DONE)**; remaining peripherals
(WDT, CRCSCAN, SLPCTRL, RSTCTRL, …) to be added as stubs then deepened.
Core: **new `simavr/cores/sim_tiny3217.c`**, **new
`cores/sim_core_declare_modern.h`**, bundled **`cores/avr/iotn3217.h`**.
Tests: `tests/test_avrxt_engine.c` (engine + TWI0 + PORT/VPORT + sim_tiny3217
wiring).
```
