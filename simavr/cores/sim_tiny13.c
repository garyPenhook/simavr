/*
	sim_tiny13.c

	Copyright 2008, 2009 Michel Pollet <buserror@gmail.com>
	                     Jon Escombe <lists@dresco.co.uk>

 	This file is part of simavr.

	simavr is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	simavr is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with simavr.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <string.h>
#include "sim_avr.h"
#include "avr_eeprom.h"
#include "avr_watchdog.h"
#include "avr_extint.h"
#include "avr_ioport.h"
#include "avr_timer.h"
#include "avr_adc.h"
#include "avr_acomp.h"
#include "avr_clkpr.h"
#include "avr_flash.h"
#include "avr_regmask.h"

#define _AVR_IO_H_
#define __ASSEMBLER__
#include "avr/iotn13a.h"		/* the 13A header, not iotn13.h: only it has PRR and BODCR */

#include "sim_core_declare.h"

static void init(struct avr_t * avr);
static void reset(struct avr_t * avr);
static int sleep_source_active(struct avr_t * avr, struct avr_int_vector_t * vector);


static const struct mcu_t {
	avr_t core;
	avr_eeprom_t 	eeprom;
	avr_watchdog_t	watchdog;
	avr_extint_t	extint;
	avr_ioport_t	portb;
	avr_timer_t		timer0;
	avr_acomp_t		acomp;
	avr_adc_t       adc;
	avr_clkpr_t	clkpr;
	avr_flash_t	selfprog;
	avr_regmask_t	regmask;
} mcu = {
	.core = {
		.mmcu = "attiny13",

		/*
		 * tiny13 has no extended fuse byte, so can not use DEFAULT_CORE macro
		 */
		.ioend  = RAMSTART - 1,
		.ramend = RAMEND,
		.flashend = FLASHEND,
		.e2end = E2END,
		.vector_size = 2,
// Disable signature when using an old avr toolchain
#ifdef SIGNATURE_0
		.signature = { SIGNATURE_0,SIGNATURE_1,SIGNATURE_2 },
		/* DS40002307A Tables 17-3/17-4: low 0x6A (SPIEN, CKDIV8, SUT0, CKSEL0 programmed), high 0xFF.
		 * The header's LFUSE_DEFAULT is not usable here: sim_core_declare.h fakes _BV(v) as v. */
		.fuse = { 0x6A, 0xFF },
#endif
		.reset_flags = {
			.porf = AVR_IO_REGBIT(MCU_STATUS_REG, PORF),
			.extrf = AVR_IO_REGBIT(MCU_STATUS_REG, EXTRF),
			.borf = AVR_IO_REGBIT(MCU_STATUS_REG, BORF),
			.wdrf = AVR_IO_REGBIT(MCU_STATUS_REG, WDRF)
		},
		/* DS40002307A 7.1: SLEEP only sleeps while MCUCR.SE is set, and the sleep mode decides
		 * which sources are still clocked */
		.sleep_enable = AVR_IO_REGBIT(MCUCR, SE),
		.sleep_source_active = sleep_source_active,
		.init = init,
		.reset = reset,
	},
	/* DS40002307A 16: SPM, a 16 word page, CTPB clears the temporary buffer, RFLB reads fuses/lock bits.
	 * SPM is disabled until the SELFPRGEN fuse (high byte, bit 4) is programmed. */
	.selfprog = {
		.flags = AVR_SELFPROG_HAVE_RWW,		/* only for the CTPB (clear temporary page buffer) hook below */
		.r_spm = SPMCSR,
		.spm_pagesize = SPM_PAGESIZE,
		.selfprgen = AVR_IO_REGBIT(SPMCSR, SPMEN),		/* the datasheet calls it SELFPRGEN */
		.pgers = AVR_IO_REGBIT(SPMCSR, PGERS),
		.pgwrt = AVR_IO_REGBIT(SPMCSR, PGWRT),
		.blbset = AVR_IO_REGBIT(SPMCSR, RFLB),
		.rwwsre = AVR_IO_REGBIT(SPMCSR, CTPB),
		.enable_fuse_byte = 1, .enable_fuse_mask = 1 << 4,
	},
	AVR_EEPROM_DECLARE_8BIT(EE_RDY_vect),
	.eeprom.timed = 1,		// DS40002307A 5.3: busy for 3.4 / 1.8 ms, EEPM modes, level-like EE_RDY
	// tiny13 has different names for these...
	#define WDIF WDTIF
	#define WDIE WDTIE
	AVR_WATCHDOG_DECLARE(WDTCR, WDT_vect),
	.watchdog.exact = 1,
	.watchdog.wdton_byte = 0, .watchdog.wdton_mask = 1 << 5,	/* WDTON is bit 5 of the low fuse byte (Table 17-4) */
	.extint = {
		AVR_EXTINT_TINY_DECLARE(0, 'B', 1, GIFR),
	},
	.portb = {
		.name = 'B',  .r_port = PORTB, .r_ddr = DDRB, .r_pin = PINB,
		.pcint = {
			.enable = AVR_IO_REGBIT(GIMSK, PCIE),
			.raised = AVR_IO_REGBIT(GIFR, PCIF),
			.vector = PCINT0_vect,
		},
		.r_pcint = PCMSK,
		.pud = AVR_IO_REGBIT(MCUCR, PUD),
		.r_didr = DIDR0,		/* 10.2.4 / 13.2.3 / 14.12.5: DIDR0 bit n disables PBn's input buffer */
	},
	.timer0 = {
		.name = '0',
		.wgm = { AVR_IO_REGBIT(TCCR0A, WGM00), AVR_IO_REGBIT(TCCR0A, WGM01), AVR_IO_REGBIT(TCCR0B, WGM02) },
		WGM_OPS_8,
		.cs = { AVR_IO_REGBIT(TCCR0B, CS00), AVR_IO_REGBIT(TCCR0B, CS01), AVR_IO_REGBIT(TCCR0B, CS02) },
		/* DS40002307A Table 11-9: 110/111 clock on the T0 pin (PB2), falling/rising edge */
		.cs_div = { 0, 0, 3 /* 8 */, 6 /* 64 */, 8 /* 256 */, 10 /* 1024 */,
					AVR_TIMER_EXTCLK_CHOOSE, AVR_TIMER_EXTCLK_CHOOSE },
		.ext_clock_pin = AVR_IO_REGBIT(PORTB, 2),

		/* datasheet-exact PWM: 510-cycle phase correct period, OCR0x latched at TOP/BOTTOM (Table 11-8) */
		.strict = 1,
		/* GTCCR.PSR10 / TSM (DS40002307A 12.4.1) */
		.psr = AVR_IO_REGBIT(GTCCR, PSR10),
		.tsm = AVR_IO_REGBIT(GTCCR, TSM),

		.r_tcnt = TCNT0,

		.overflow = {
			.enable = AVR_IO_REGBIT(TIMSK0, TOIE0),
			.raised = AVR_IO_REGBIT(TIFR0, TOV0),
			.vector = TIM0_OVF_vect,
		},
		.comp = {
			[AVR_TIMER_COMPA] = {
				.r_ocr = OCR0A,
				.com = AVR_IO_REGBITS(TCCR0A, COM0A0, 0x3),
				.com_pin = AVR_IO_REGBIT(PORTB, 0),
				.foc = AVR_IO_REGBIT(TCCR0B, FOC0A),
				.interrupt = {
					.enable = AVR_IO_REGBIT(TIMSK0, OCIE0A),
					.raised = AVR_IO_REGBIT(TIFR0, OCF0A),
					.vector = TIM0_COMPA_vect,
				}
			},
			[AVR_TIMER_COMPB] = {
				.r_ocr = OCR0B,
				.com = AVR_IO_REGBITS(TCCR0A, COM0B0, 0x3),
				.com_pin = AVR_IO_REGBIT(PORTB, 1),
				.foc = AVR_IO_REGBIT(TCCR0B, FOC0B),
				.interrupt = {
					.enable = AVR_IO_REGBIT(TIMSK0, OCIE0B),
					.raised = AVR_IO_REGBIT(TIFR0, OCF0B),
					.vector = TIM0_COMPB_vect,
				}
			}
		}
	},

	.acomp = {
		.mux_inputs = 4,
		.mux = { AVR_IO_REGBIT(ADMUX, MUX0), AVR_IO_REGBIT(ADMUX, MUX1) },
		.aden = AVR_IO_REGBIT(ADCSRA, ADEN),
		.acme = AVR_IO_REGBIT(ADCSRB, ACME),

		.r_acsr = ACSR,
		.acis = { AVR_IO_REGBIT(ACSR, ACIS0), AVR_IO_REGBIT(ACSR, ACIS1) },
		.aco = AVR_IO_REGBIT(ACSR, ACO),
		.acbg = AVR_IO_REGBIT(ACSR, ACBG),
		.disabled = AVR_IO_REGBIT(ACSR, ACD),

		.ac = {
			.enable = AVR_IO_REGBIT(ACSR, ACIE),
			.raised = AVR_IO_REGBIT(ACSR, ACI),
			.vector = ANA_COMP_vect,
		}
	},

	.adc = {
		.r_admux = ADMUX,
		.mux = { AVR_IO_REGBIT(ADMUX, MUX0),
			 AVR_IO_REGBIT(ADMUX, MUX1), },
		.ref = { AVR_IO_REGBIT(ADMUX, REFS0), },
		.ref_values = {
			[0] = ADC_VREF_VCC, [1] = ADC_VREF_V110,
		},

		.adlar = AVR_IO_REGBIT(ADMUX, ADLAR),
		.r_adcsra = ADCSRA,
		.aden = AVR_IO_REGBIT(ADCSRA, ADEN),
		.adsc = AVR_IO_REGBIT(ADCSRA, ADSC),
		.adate = AVR_IO_REGBIT(ADCSRA, ADATE),
		.adps = { AVR_IO_REGBIT(ADCSRA, ADPS0),
			  AVR_IO_REGBIT(ADCSRA, ADPS1),
			  AVR_IO_REGBIT(ADCSRA, ADPS2),},

		.r_adch = ADCH,
		.r_adcl = ADCL,

		.r_adcsrb = ADCSRB,
		.adts = { AVR_IO_REGBIT(ADCSRB, ADTS0),
			  AVR_IO_REGBIT(ADCSRB, ADTS1),
			  AVR_IO_REGBIT(ADCSRB, ADTS2),},
		.adts_op = {
			[0] = avr_adts_free_running,
			[1] = avr_adts_analog_comparator_0,
			[2] = avr_adts_external_interrupt_0,
			[3] = avr_adts_timer_0_compare_match_a,
			[4] = avr_adts_timer_0_overflow,
			[5] = avr_adts_timer_0_compare_match_b,
			[6] = avr_adts_pin_change_interrupt,
		},
		.exact = 1,	/* 14.11: ADC = VIN * 1024 / VREF */
		/* DS40002307A Table 14-5: the trigger is the rising edge of the source's interrupt flag */
		.adts_vec = {
			[1] = ANA_COMP_vect,
			[2] = INT0_vect,
			[3] = TIM0_COMPA_vect,
			[4] = TIM0_OVF_vect,
			[5] = TIM0_COMPB_vect,
			[6] = PCINT0_vect,
		},
		.muxmode = {
			[0] = AVR_ADC_SINGLE(0), [1] = AVR_ADC_SINGLE(1),
			[2] = AVR_ADC_SINGLE(2), [3] = AVR_ADC_SINGLE(3),
		},

		.adc = {
			.enable = AVR_IO_REGBIT(ADCSRA, ADIE),
			.raised = AVR_IO_REGBIT(ADCSRA, ADIF),
			.vector = ADC_vect,
		},
	},
};

/*
 * Reserved bits "always read as zero" (DS40002307A section 20 register summary and the register
 * descriptions), MCUSR flags that software can only clear (8.5.1), and the BODCR timed sequence (7.5.1).
 */
#define _REGMASK(_reg, _mask, ...) { .reg = (_reg), .mask = (_mask), ##__VA_ARGS__ }

static void init_regmask(avr_t * avr)
{
	struct mcu_t * mcu = (struct mcu_t*)avr;
	static const avr_regmask_entry_t entries[] = {
		_REGMASK(PORTB, 0x3f), _REGMASK(DDRB, 0x3f), _REGMASK(PCMSK, 0x3f),
		_REGMASK(GIMSK, 0x60), _REGMASK(GIFR, 0x60), _REGMASK(MCUCR, 0x7b),
		_REGMASK(TIMSK0, 0x0e), _REGMASK(TIFR0, 0x0e),
		_REGMASK(TCCR0A, 0xf3), _REGMASK(TCCR0B, 0x0f),		/* FOC0A/B are strobes, read as zero */
		_REGMASK(EECR, 0x3f), _REGMASK(EEAR, 0x3f),
		_REGMASK(ADMUX, 0x63), _REGMASK(ADCSRB, 0x47), _REGMASK(ACSR, 0xfb),
		_REGMASK(DIDR0, 0x3f), _REGMASK(GTCCR, 0x81), _REGMASK(PRR, 0x03),
		_REGMASK(MCUSR, 0x0f, .kind = AVR_REGMASK_CLEAR_ONLY),
		_REGMASK(BODCR, 0x03, .kind = AVR_REGMASK_BODCR),
	};
	memcpy(mcu->regmask.entry, entries, sizeof(entries));
	avr_regmask_init(avr, &mcu->regmask);
}

static avr_t * make()
{
	return avr_core_allocate(&mcu.core, sizeof(struct mcu_t));
}

avr_kind_t tiny13 = {
	.names = { "attiny13", "attiny13a" },
	.make = make
};

static void init(struct avr_t * avr)
{
	struct mcu_t * mcu = (struct mcu_t*)avr;

	avr_flash_init(avr, &mcu->selfprog);
	avr_eeprom_init(avr, &mcu->eeprom);
	avr_watchdog_init(avr, &mcu->watchdog);
	avr_extint_init(avr, &mcu->extint);
	avr_ioport_init(avr, &mcu->portb);
	avr_timer_init(avr, &mcu->timer0);
	avr_acomp_init(avr, &mcu->acomp);
	avr_adc_init(avr, &mcu->adc);
	avr_clkpr_init(avr, &mcu->clkpr, CLKPR, 0, 4 /* CKDIV8, low fuse bit 4 */);
	init_regmask(avr);		/* last: the reserved-bit masks go on top of the models above */
}

/*
 * DS40002307A Table 7-1 / 7.1: which interrupt sources are still clocked in the selected sleep
 * mode. A source that is not can not set its flag or wake the CPU. INT0 only works as a level
 * interrupt outside Idle (edge detection needs clkI/O).
 */
static int sleep_source_active(struct avr_t * avr, struct avr_int_vector_t * vector)
{
	avr_regbit_t sm0 = AVR_IO_REGBIT(MCUCR, SM0), sm1 = AVR_IO_REGBIT(MCUCR, SM1);
	avr_regbit_t isc0 = AVR_IO_REGBIT(MCUCR, ISC00), isc1 = AVR_IO_REGBIT(MCUCR, ISC01);
	int mode = avr_regbit_get(avr, sm0) | (avr_regbit_get(avr, sm1) << 1);

	if (mode == 0 || mode == 3)		/* Idle (0b11 is reserved) */
		return 1;
	switch (vector->vector) {
		case WDT_vect:
		case PCINT0_vect:
			return 1;
		case INT0_vect:
			return !avr_regbit_get(avr, isc0) && !avr_regbit_get(avr, isc1);
		case ADC_vect:
		case EE_RDY_vect:
			return mode == 1;		/* ADC Noise Reduction only */
		default:
			return 0;
	}
}

static void reset(struct avr_t * avr)
{
//	struct mcu_t * mcu = (struct mcu_t*)avr;
}
