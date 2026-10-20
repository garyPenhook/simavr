# ATtiny13A datasheet audit

Source of truth: ATtiny13A data sheet DS40002307A. Every item below was checked against the
section named in brackets; the checks that survive as regression tests are in
`tests/test_attiny13a_audit.c` (host-only, no avr-gcc: `make -C tests run_tests`).

The classic engine is frozen for regression safety, so datasheet-exact behaviour that changes what
other classic cores do is **opt-in per core** (a flag on the peripheral, set in `sim_tiny13.c`).
Fixes for plain bugs that the datasheets of every classic part agree on are global, and listed as such.

## Opt-in flags (only the ATtiny13A sets them)

| Flag | Peripheral | What it switches on |
| --- | --- | --- |
| `avr_timer_t.strict` | Timer0 | FOC0x never stored and ignored in PWM modes, CTC sets TOV on MAX only [Table 11-8], 510-cycle phase correct period, OCR0x latched at TOP / BOTTOM (Table 11-8), reserved COM0x encodings disconnected, a TCNT0 write blocking the next compare match, OCR0x = MAX in fast PWM holding the output |
| `avr_timer_t.psr / .tsm` | Timer0 | GTCCR PSR10 / TSM [12.4.1] |
| `avr_eeprom_t.timed` | EEPROM | EEPE busy for 3.4 / 1.8 ms, EEPM erase-only / write-only, level-like EE_RDY, 4 / 2 cycle CPU halt [5.3, 5.5.3] |
| `avr_adc_t.exact` | ADC | 13 / 25 ADC clocks per conversion (the original model takes about twice as long, and truncates at ADPS = 7) [Table 14-1]; single-ended result `VIN * 1024 / VREF` (the original scales by 1023) [14.11] |
| `avr_watchdog_t.exact` | WDT | WDIF cleared by writing a one; a time-out with the previous interrupt unserviced in Interrupt and System Reset mode resets [8.5.2] |
| `avr_adc_t.adts_vec[]` | ADC | auto trigger from the rising edge of an interrupt flag [Table 14-5] |
| `avr_t.sleep_enable / .sleep_source_active` | CPU | SLEEP honours `SE`; sources whose clock is stopped by the sleep mode do not set flags or wake [7.1, Table 7-1] |
| `avr_ioport_t.r_didr` | Port B | a set DIDR0 bit makes the same PINB bit read 0 [10.2.4, 13.2.3] |
| `avr_watchdog_t.wdton_*` | WDT | WDTON fuse forces System Reset mode [8.4, Table 17-4] |
| `avr_flash_t.enable_fuse_*` | SPM | SPM is disabled until SELFPRGEN is programmed [16] |

## Fixed for every classic core

* **Core list** (`sim_host.c`): `avr_host_core_name()` returned only the first name of each core, so
  aliases such as `attiny13a` never reached the GUI's MCU picker. All names are listed now.
* **Fast PWM with OCRnA as TOP** (modes 7 / 15): the timer's TOP stayed 0 and the counter never ran.
* **FOCnx registration**: registered once per register (it strobed twice when FOC0A and FOC0B share one).
* **Shared-IO table**: `io_shared_io` grew from
  4 to 24 slots (an ABI change: rebuild anything compiled against the old `avr_t`).

## Per peripheral

| Peripheral | Result |
| --- | --- |
| CPU, SREG, instruction timing | classic AVRe timing already matched Table 21 |
| Interrupt vectors (Table 9-1) | 10 vectors, 2-byte stride: matched |
| Registers | reserved bits now read 0 (`avr_regmask`); MCUSR flags only clear; BODCR timed sequence |
| Port B | PUD wired (was missing); PINB toggle, DIDR0 added; PORTB/DDRB/PCMSK limited to 6 pins |
| INT0 / PCINT | edge and level modes and the output-pin software interrupt already matched |
| Timer0 | see the opt-in flags; external clock on T0 (CS = 110 / 111) was not wired; `cs_div[6..7]` fell back to clk/1 |
| Watchdog | W1C, reset-after-unserviced-interrupt, WDTON fuse; the timed WDCE sequence already matched |
| EEPROM | busy time, EEPM, EERIE, halt cycles; address register limited to 6 bits |
| ADC | timing, scaling, six auto trigger sources; channel map, REFS0 / 1.1 V, ADLAR, ADIF / ADSC already matched |
| Analog comparator | ACIS modes, ACBG (1.1 V), ACME multiplexer already matched |
| Sleep | SE gating, SM1:0, per-mode wake sources |
| Self-programming | not modelled before; SPMCSR / SPM page erase / fill / write / CTPB / RFLB, behind the SELFPRGEN fuse |
| Core header | `sim_tiny13.c` now includes `avr/iotn13a.h`: only it defines PRR and BODCR |

## Known gaps (not modelled)

* While the CPU sleeps in ADC Noise Reduction / Power-down the Timer0 *counter* keeps advancing
  (its flags and wake-ups are suppressed, as the datasheet requires, but TCNT0 is not frozen).
* ADC Noise Reduction does not start a conversion automatically on SLEEP [7.1.2].
* A PUD change does not pull an already-high floating input low; the pull-up is only withheld when the
  pin is next updated.
* SPM does not halt the CPU or take the page programming time; an EEPROM write does not block SPMCSR [16.5].
* PRR does not freeze the peripherals; OSCCAL resets to 0 rather than the calibration byte; the
  4.8 / 9.6 MHz oscillator selection and BOD levels are not modelled.
* With T0 clocking (CS = 110 / 111) Timer0 uses the original external-clock counter, which has no
  dual slope or OCR0x buffering.
* PSR10 is a no-op on a phase correct down slope.
* ACO is writable; ADLAR takes effect at the next conversion rather than immediately [14.12.1].
* JMP / CALL / MUL and friends, absent from the ATtiny13A's instruction set, still execute.
* An ADC conversion's sample-and-hold is 2 ADC clocks after the trigger for every start, not 1.5 / 2
  as in Table 14-1 (total conversion times match).
