# AVR DD (AVR Dx) support

Status of the AVR DD family in this fork. Source of truth for every register,
address and behaviour below is the *AVR16/32DD14/20 Complete Data Sheet*
(DS40002413B) and the errata DS80001026; the device header is the one from the
Microchip AVR-Dx DFP (2.8.343), vendored as `simavr/cores/avr/ioavr32dd14.h`.

## Devices

| Device     | Core            | State |
|------------|-----------------|-------|
| AVR32DD14  | `avr32dd14`     | boots and runs firmware, partial peripheral set |

The remaining DD parts (AVR16/32DD20, AVR16/32DD28/32, AVR64DD14..32) need only a
device file plus their header, but the 28/32-pin and 64 KB parts also have PORTE/
PORTB-less variants and a second 32 KB flash window (`NVMCTRL.CTRLB.FLMAP`), which
is not modelled; they are not registered yet.

## Modelled

* **Memory map** (8.2, 9.1): flash mapped at `0x8000`, EEPROM `0x1400`, USERROW
  `0x1080`, SIGROW `0x1100`, SRAM `0x7000-0x7FFF` (4 KB), 4-byte `JMP` vectors.
* **CLKCTRL** (`avr_clkctrl_dd.c`, 12): OSCHF `FRQSEL` 1-24 MHz (4 MHz at reset),
  OSC32K, XOSC32K, `EXTCLK`, the `PDIV` prescaler (2..64, 6/10/12/24/48),
  `FUSE.OSCCFG` start-up clock, `MCLKSTATUS`, CCP on every protected register. A
  switch to a not-yet-enabled external source stays pending (`SOSC`). OSCHF and PLL
  (2x/3x) frequencies are available to TCD0.
* **NVMCTRL** (`avr_nvmctrl_dd.c`, 11): command-then-store model (`FLWR`, `FLPER`,
  `FLMPERn`, `EEWR`, `EEERWR`, `EEBER`, `EEMBERn`, `EECHER`), USERROW writes and
  erase, `STATUS.ERROR`, BOOT/APPCODE/APPDATA sections from `FUSE.BOOTSIZE` and
  `FUSE.CODESIZE` with the inter-section write protection of Table 11-1, the
  `CTRLB` write-protect bits, level `EEREADY` interrupt. EEPROM and USERROW persist
  across reset.
* **PORT** multi-pin configuration (`PINCONFIG` mirrored across ports,
  `PINCTRLUPD/SET/CLR`, 18.3.2.4).
* **EVSYS** at `0x200` (6 channels, 25 users, 16.5); generators: AC0 OUT, ADC0
  RESRDY, ZCD3 OUT, MVIO VDDIO2OK, CCL LUT0-3, TCB0/1 CAPT; users: ADC0 START, TCA0
  CNTA, TCB0/1 CAPT.
* **PORTMUX** at `0x5E0` (offsets 0-9): USART0/1 TxD (`USARTROUTEA`) and TCA0 WO0-5
  (`TCAROUTEA`) are routed to their pins.
* **VREF** (21): `ADC0REF`, `DAC0REF`, `ACREF` decoded to millivolts and fed to
  ADC0, DAC0 and the AC0 DACREF divider; republished on reset and when the host
  changes the VDD / VREFA level (`avr_vref_set_vdd()` / `avr_vref_set_vrefa()`; VREFA
  defaults to 0 mV because the data sheet gives no value).
* **AC0** (`avr_ac_dd.c`, 32): AINP3/4, AINN2/3, DACREF, INVERT, HYSMODE (typical
  VHYST of Table 38-26), `INTCTRL.INTMODE`, `CMPIF`/`CMPSTATE`.
* **DAC0** (34): 10-bit left-aligned `DATA`, output on `DATAH` write.
* **ZCD3** (35): switching level ZCPINV = 0.95 V, `INVERT`, `INTMODE`, `CROSSIF`.
* **MVIO** (19): `VDDIO2S`/`VDDIO2IF` driven by an input IRQ in the dual-supply
  fuse configuration, always OK in single-supply.
* **ADC0** (`avr_adc_dd.c`, 33): single-ended/differential 12/10-bit, accumulation
  with the Table 33-1/33-2 formats, free-running, `STCONV`/`SPCONV`, event start,
  window comparator, `RES` read clears the flags, and the 33.3.3.4 conversion time.
  Inputs: AINn, GND, TEMPSENSE (Kelvin), VDD/10, VDDIO2/10, DAC0, DACREF0.
* **TCD0** (`avr_tcd.c`): clock source (OSCHF / PLL / EXTCLK / SYSCLK) follows
  CLKCTRL; WOC / WOD mirror WOA or WOB (`CTRLC.CMPCSEL` / `CMPDSEL`, enabled by
  `FAULTCTRL.CMPCEN` / `CMPDEN`); `STATUS.PWMACTA/B`; the CMPBCLR / CMPASET /
  CMPBSET compare-match events (EVSYS generators 0xB0-0xB2); event inputs A / B
  (EVSYS users 23 / 24) with `EVCTRLx.TRIGEI` / `EDGE` flagging `INTFLAGS.TRIGA/B`
  and the `TRIG` interrupt (vector 17); WOA/WOB/WOC feed the CCL.
* **TWI0 dual mode** (29.3.3.4): `CTRLA` and `DUALCTRL` register fields and
  `DUALCTRL.ENABLE`; host and client already run independently, and the pin sets
  are not modelled.
* **TCD0 capture, input modes, blanking, PROGEV** (25.3.3.4, 25.3.3.6, 25.3.4.1):
  `CTRLE.SCAPTUREA/B` and event capture (`EVCTRLx.ACTION`) into `CAPTUREA/B`, blocked
  until the high byte is read; `INPUTMODE` 0, 4, 7 (stop until `CTRLE.RESTART`), 9 and
  10, forcing the outputs to the `FAULTCTRL.CMPx` level; input-A blanking and the
  programmable event output (`DLYCTRL`/`DLYVAL`, EVSYS generator 0xB3).
  Counter-flow modes 1, 2, 3, 5, 6 and 8 (Table 25-6) run on a four-segment model of
  the cycle (DTA, OTA, DTB, OTB); the digital filter (`EVCTRLx.CFG = FILTERON`, four
  counter clocks); dithering (`DITCTRL` / `DITVAL`, Table 25-7, as extra counts at the
  end of the cycle); `DBGCTRL.DBGRUN` / `FAULTDET` through `avr_tcd_debug_halt()`.
* **CRCSCAN** (`avr_crcscan.c`): CRC16 and CRC32 with the 512-byte `BOOTSIZE`/`CODESIZE`
  units and the Dx `CRCSRC` / `CRCSEL` fuse encoding.
* **CCL** input sources (31.5.7): AC0, ZCD3, TCA0 WO0-2, TCB0/1 WO.
* **SYSCFG / SIGROW / FUSE / LOCK** (`avr_syscfg_dd.c`): `REVID`, device ID,
  `TEMPSENSEn`, 16-byte serial number, read-only fuse window with the factory
  defaults of 8.8.2.
* **CPUINT** vector base from `FUSE.BOOTSIZE` in 512-byte units.
* Re-used AVRxt blocks whose register layouts are identical to the tinyAVR
  2-series (checked register-by-register against the device files): PORT/VPORT
  A, C, D, F; TCA0; TCB0-1; USART0-1; SPI0; RTC/PIT; WDT; BOD; SLPCTRL; RSTCTRL;
  TWI0.

## Not modelled yet (registers read and write as plain memory or are inert)

CCL inputs from USART TxD and SPI (no line-level IRQs), TCD0 synchroniser latency
(2-3 CLK_TCD_SYNC) and the separate asynchronous output override (`EVCTRLx.CFG =
ASYNCON` acts like the default, immediately), the GDB stub calling
`avr_tcd_debug_halt()` (simavr has no debug-halt notification), CLKCTRL PLL as a main-clock source / auto-tune / clock failure detection,
`SLPCTRL.VREGCTRL`, SPM decoding, `BOOTRP` read protection, `FLMAP` for parts with
more than 32 KB of flash, MVIO pin tri-stating, ADC `RUNSTDBY` and the
PRESC-dependent CLK_ADC limits, AC `INITVAL` and POWER response times.

## Known limitations

The PLL is used without modelling lock; TCD0 does not re-time its counter when the
clock changes while it runs; the TCD0 compare-match events are generated from the
counter boundaries the timer already steps through, so their exact cycle is
approximate; reserved `REFSEL` values publish 0 mV.

## Simplifications

Stores to flash/EEPROM take effect immediately; `STATUS.FBUSY/EEBUSY` is held for a
nominal number of cycles and the CPU is never halted. A store replaces the old
contents instead of clearing bits only. `CHER` is ignored when issued by the CPU
(it is UPDI-only). The `REVID` and temperature-sensor calibration values are
placeholders (the temperature-sensor words are chosen so the ADC result is
3584 - T[K]).

**Unverified:** DS40002413B gives the CRC start value only for CRC16 (0xFFFF). The
CRC32 scan assumes the IEEE 802.3 polynomial shifted in MSB first (30.3.2), start
value 0xFFFFFFFF and no final XOR, with the checksum little-endian at the end of the
section (Table 30-2). If real images use the reflected zlib variant, a valid CRC32
image would fail the scan.

TCD0 extras: a blanked input-A event is ignored entirely (its level change is not
re-evaluated when the window closes); the blanking / PROGEV delay is the data-sheet
`DLYPRESC * DLYVAL` CLK_TCD_SYNC periods rounded to whole CLK_PER cycles;
`DLYTRIG` value 3 is treated as CMPBCLR (the data sheet table text says CMPASET);
CMPACLR is signalled at the phase boundary in Two / Four Ramp mode.

TCD0 flow modes are built from the text and Table 25-6 only (the timing diagrams are
not in the text extraction): mode 6 from an on-time behaves like mode 1 on both
outputs, and from a dead-time waits at the next on-time start, resuming with the
dead-time of the other compare. Dither only lengthens the cycle by the Table 25-7
count; the extra counts are not placed in the selected state, `DITHER` is not
double-buffered, and the compare registers are not double-buffered either.

## Testing

`tests/test_avrxt_engine.c` has `AVR DD (AVR32DD14) core`, `AVR DD CLKCTRL`, `AVR DD
PORT multi-pin configuration`, `EVSYS / PORTMUX`, `VREF / AC / DAC / ZCD / MVIO`,
`TCD0 clock sources`, `ADC0`, `CCL input sources`, `TWI dual mode / TCD0 outputs, events, TRIG`, `CRCSCAN` and `NVMCTRL` groups. A firmware smoke test
builds with

    D=~/.mchp_packs/Microchip/AVR-Dx_DFP/2.8.343
    avr-gcc -mmcu=avr32dd14 -B $D/gcc/dev/avr32dd14 -I $D/include -Os -o t.elf t.c
    simavr/run_avr -m avr32dd14 t.elf
