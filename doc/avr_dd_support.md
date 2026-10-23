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
  switch to a not-yet-enabled external source stays pending (`SOSC`).
* **NVMCTRL** (`avr_nvmctrl_dd.c`, 11): command-then-store model (`FLWR`, `FLPER`,
  `FLMPERn`, `EEWR`, `EEERWR`, `EEBER`, `EEMBERn`, `EECHER`), USERROW writes and
  erase, `STATUS.ERROR`, BOOT/APPCODE/APPDATA sections from `FUSE.BOOTSIZE` and
  `FUSE.CODESIZE` with the inter-section write protection of Table 11-1, the
  `CTRLB` write-protect bits, level `EEREADY` interrupt. EEPROM and USERROW persist
  across reset.
* **PORT** multi-pin configuration (`PINCONFIG` mirrored across ports,
  `PINCTRLUPD/SET/CLR`, 18.3.2.4).
* **SYSCFG / SIGROW / FUSE / LOCK** (`avr_syscfg_dd.c`): `REVID`, device ID,
  `TEMPSENSEn`, 16-byte serial number, read-only fuse window with the factory
  defaults of 8.8.2.
* **CPUINT** vector base from `FUSE.BOOTSIZE` in 512-byte units.
* Re-used AVRxt blocks whose register layouts are identical to the tinyAVR
  2-series (checked register-by-register against the device files): PORT/VPORT
  A, C, D, F; TCA0; TCB0-1; USART0-1; SPI0; RTC/PIT; WDT; BOD; CCL (4 LUTs);
  SLPCTRL; RSTCTRL; TWI0.

## Not modelled yet (registers read and write as plain memory)

EVSYS (different generator/user map), PORTMUX, VREF (`ADC0REF/DAC0REF/ACREF`),
ADC0 (12-bit differential, `adc_12b_diff_ctrl_v1`), AC0 (`MUXCTRL`), DAC0, ZCD3,
MVIO, TCD0, CRCSCAN (the Dx sections are 512-byte units), TWI dual mode, CLKCTRL
PLL output / auto-tune / clock failure detection, `SLPCTRL.VREGCTRL`, SPM
decoding, `BOOTRP` read protection, CCL input sources, `FLMAP` for parts with more
than 32 KB of flash.

## Simplifications

Stores to flash/EEPROM take effect immediately; `STATUS.FBUSY/EEBUSY` is held for a
nominal number of cycles and the CPU is never halted. A store replaces the old
contents instead of clearing bits only. `CHER` is ignored when issued by the CPU
(it is UPDI-only). The `REVID` and temperature-sensor calibration values are
placeholders.

## Testing

`tests/test_avrxt_engine.c` has `AVR DD (AVR32DD14) core`, `AVR DD CLKCTRL`, `AVR DD
PORT multi-pin configuration` and `AVR DD NVMCTRL` groups. A firmware smoke test
builds with

    D=~/.mchp_packs/Microchip/AVR-Dx_DFP/2.8.343
    avr-gcc -mmcu=avr32dd14 -B $D/gcc/dev/avr32dd14 -I $D/include -Os -o t.elf t.c
    simavr/run_avr -m avr32dd14 t.elf
