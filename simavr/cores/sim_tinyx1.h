/*
	sim_tinyx1.h

	Shared core template for the Microchip tinyAVR(R) 1-series (AVRxt). Every
	1-series part has the same peripheral architecture and register map; they
	differ only in memory sizes, signature, the interrupt vector table, and which
	*instances* of a peripheral are fitted (PORTB/PORTC by pin count; TCB1, ADC1,
	AC1/AC2 on the larger parts). All of that is taken straight from the device's
	own avr-libc header, which is generated from the same Microchip device files
	as the datasheets — so each core is "designed by the datasheet of each".

	A concrete core (cores/sim_tinyNNN.c) is then just:

		#include "sim_avr.h"
		#define SIM_MMCU     "attinyNNN"
		#define SIM_CORENAME mcu_tinyNNN
		#define SIM_KIND     tinyNNN
		#define _AVR_IO_H_
		#define __ASSEMBLER__
		#define _SFR_MEM8(x)  (x)
		#define _SFR_MEM16(x) (x)
		#define _SFR_IO8(x)   (x)
		#include "avr/iotnNNN.h"
		#include "sim_tinyx1.h"

	Copyright 2026 simavr authors

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

#if !defined(SIM_MMCU) || !defined(SIM_CORENAME)
#error "include the device header and define SIM_MMCU/SIM_CORENAME first"
#endif

/*
 * Vector entry size: parts with >8 KB flash use 4-byte JMP vectors, smaller
 * parts 2-byte RJMP vectors (matches __AVR_HAVE_JMP_CALL__ / avr-gcc output).
 */
#if FLASHEND > 0x1FFF
#define SIM_VECTOR_SIZE	4
#else
#define SIM_VECTOR_SIZE	2
#endif

/* EEPROM size from the device's mapped EEPROM window. */
#define SIM_EE_SIZE	(E2END - EEPROM_START + 1)

/*
 * The device header above was parsed with identity _SFR_xxx, _BV and _VECTOR
 * macros (the including .c provides them, since avr/io.h is blocked).
 * sim_core_declare.h, included next, defines its own, so drop ours to avoid a
 * -Werror redefinition. Only the device header's plain integer macros (sizes,
 * signature, vector numbers) are used from here on, so these are done with.
 */
#undef _SFR_IO8
#undef _SFR_IO16
#undef _SFR_MEM8
#undef _BV
#undef _VECTOR

#include "sim_core_declare_modern.h"
#include "avr_clkctrl.h"
#include "avr_tca.h"
#include "avr_tcb.h"
#include "avr_twi_modern.h"
#include "avr_usart_modern.h"
#include "avr_nvmctrl.h"
#include "avr_port_modern.h"
#include "avr_rtc.h"
#include "avr_adc_modern.h"
#include "avr_spi_modern.h"
#include "avr_ac.h"
#include "avr_dac.h"
#include "avr_ccl.h"
#include "avr_evsys.h"
#include "avr_portmux.h"
#include "avr_vref.h"
#include "avr_tcd.h"
#include "avr_wdt.h"
#include "avr_crcscan.h"
#include "avr_slpctrl.h"
#include "avr_cpuint.h"
#include "avr_rstctrl.h"
#include "avr_bod.h"
#include "avr_syscfg.h"

/*
 * The device structure. The always-present peripherals match every 1-series
 * part; PORTB/PORTC, TCB1, ADC1 and AC1/AC2 are only included when the device
 * header declares their interrupt vector (i.e. the part actually has them).
 */
struct mcu_t {
	avr_t				core;
	avr_clkctrl_t		clkctrl;
	avr_nvmctrl_t		nvmctrl;
	avr_port_modern_t	porta;
#ifdef PORTB_PORT_vect_num
	avr_port_modern_t	portb;
#endif
#ifdef PORTC_PORT_vect_num
	avr_port_modern_t	portc;
#endif
	avr_tca_t			tca0;
	avr_tcb_t			tcb0;
#ifdef TCB1_INT_vect_num
	avr_tcb_t			tcb1;
#endif
	avr_usart_modern_t	usart0;
	avr_twi_modern_t	twi;
	avr_rtc_t			rtc;
	avr_adc_modern_t	adc0;
#ifdef ADC1_RESRDY_vect_num
	avr_adc_modern_t	adc1;
#endif
	avr_spi_modern_t	spi0;
	avr_ac_t			ac0;
#ifdef AC1_AC_vect_num
	avr_ac_t			ac1;
#endif
#ifdef AC2_AC_vect_num
	avr_ac_t			ac2;
#endif
	avr_dac_t			dac0;
	avr_ccl_t			ccl;
	avr_evsys_t			evsys;
	avr_portmux_t		portmux;
	avr_vref_t			vref;
	avr_tcd_t			tcd0;
	avr_wdt_modern_t	wdt;
	avr_crcscan_t		crcscan;
	avr_slpctrl_t		slpctrl;
	avr_cpuint_t		cpuint;
	avr_rstctrl_t		rstctrl;
	avr_bod_t			bod;
	avr_syscfg_t		syscfg;
};

/*
 * On-chip routing: the DAC0 output feeds AC0's "DAC" negative input. Mirror that
 * by updating AC0's DAC reference whenever DAC0's output (millivolts) changes.
 */
static void
tinyx1_dac_to_ac(struct avr_irq_t * irq, uint32_t value, void * param)
{
	avr_ac_t * ac = (avr_ac_t *)param;
	(void)irq;
	avr_ac_set_refs(ac, ac->vref_mv, value);
}

/*
 * VREF.CTRLA.DAC0REFSEL selects the internal reference shared by DAC0 and AC0;
 * push the decoded reference (millivolts) to both. VREF.ADC0REFSEL is wired
 * separately to ADC0.
 */
static void
tinyx1_vref_to_dac_ac(struct avr_irq_t * irq, uint32_t value, void * param)
{
	struct mcu_t * mcu = (struct mcu_t *)param;
	(void)irq;
	avr_dac_set_vref(&mcu->dac0, value);
	avr_ac_set_refs(&mcu->ac0, value, mcu->ac0.dacref_mv);
}

/* VREF.ADC0REFSEL -> ADC0 internal reference (used when CTRLC.REFSEL = INTREF). */
static void
tinyx1_vref_to_adc(struct avr_irq_t * irq, uint32_t value, void * param)
{
	struct mcu_t * mcu = (struct mcu_t *)param;
	(void)irq;
	avr_adc_modern_set_intref(&mcu->adc0, value);
}

#ifdef ADC1_RESRDY_vect_num
/* VREF.CTRLC.ADC1REFSEL -> ADC1 internal reference (16K/32K parts). */
static void
tinyx1_vref_to_adc1(struct avr_irq_t * irq, uint32_t value, void * param)
{
	struct mcu_t * mcu = (struct mcu_t *)param;
	(void)irq;
	avr_adc_modern_set_intref(&mcu->adc1, value);
}
#endif

#ifdef AC1_AC_vect_num
/* VREF.CTRLC.DAC1REFSEL -> AC1 reference (DAC1 absent on tinyAVR-1). */
static void
tinyx1_vref_to_ac1(struct avr_irq_t * irq, uint32_t value, void * param)
{
	struct mcu_t * mcu = (struct mcu_t *)param;
	(void)irq;
	avr_ac_set_refs(&mcu->ac1, value, mcu->ac1.dacref_mv);
}
#endif

#ifdef AC2_AC_vect_num
/* VREF.CTRLD.DAC2REFSEL -> AC2 reference (DAC2 absent on tinyAVR-1). */
static void
tinyx1_vref_to_ac2(struct avr_irq_t * irq, uint32_t value, void * param)
{
	struct mcu_t * mcu = (struct mcu_t *)param;
	(void)irq;
	avr_ac_set_refs(&mcu->ac2, value, mcu->ac2.dacref_mv);
}
#endif

/*
 * EVSYS routing: AC0's output is an async event generator (source AC0_OUT =
 * 0x03); forward its level changes to EVSYS. The ADC0 EVSYS user delivers its
 * channel to the ADC event-start input (honoured when ADC EVCTRL.STARTEI is set).
 */
static void
tinyx1_ac0_to_evsys(struct avr_irq_t * irq, uint32_t value, void * param)
{
	struct mcu_t * mcu = (struct mcu_t *)param;
	(void)irq;
	avr_evsys_async_generator(&mcu->evsys, 0x03 /* AC0_OUT */, value & 1);
}

static void
tinyx1_evsys_to_adc(struct avr_irq_t * irq, uint32_t value, void * param)
{
	struct mcu_t * mcu = (struct mcu_t *)param;
	(void)irq;
	if (value & 1)			/* rising event edge starts a conversion */
		avr_adc_modern_event_start(&mcu->adc0);
}

static void
tinyx1_tcb0_capt_to_evsys(struct avr_irq_t * irq, uint32_t value, void * param)
{
	struct mcu_t * mcu = (struct mcu_t *)param;
	(void)irq;
	avr_evsys_async_generator(&mcu->evsys, 0x01 /* SYNCCHx.TCB0 */, value & 1);
}

#ifdef TCB1_INT_vect_num
static void
tinyx1_tcb1_capt_to_evsys(struct avr_irq_t * irq, uint32_t value, void * param)
{
	struct mcu_t * mcu = (struct mcu_t *)param;
	(void)irq;
	avr_evsys_async_generator(&mcu->evsys, 0x15 /* SYNCCHx.TCB1 */, value & 1);
}
#endif

static void
tinyx1_evsys_to_tcb0(struct avr_irq_t * irq, uint32_t value, void * param)
{
	struct mcu_t * mcu = (struct mcu_t *)param;
	(void)irq;
	avr_raise_irq(avr_io_getirq(&mcu->core, AVR_IOCTL_TCB_GETIRQ('0'),
								AVR_TCB_IRQ_EVENT_IN), value & 1);
}

#ifdef TCB1_INT_vect_num
static void
tinyx1_evsys_to_tcb1(struct avr_irq_t * irq, uint32_t value, void * param)
{
	struct mcu_t * mcu = (struct mcu_t *)param;
	(void)irq;
	avr_raise_irq(avr_io_getirq(&mcu->core, AVR_IOCTL_TCB_GETIRQ('1'),
								AVR_TCB_IRQ_EVENT_IN), value & 1);
}
#endif

/*
 * On-chip routing: a BOD brown-out (VDD below the configured BOD level) resets
 * the device through RSTCTRL, which records the cause in RSTFR.BORF.
 */
static void
tinyx1_bod_brownout(struct avr_t * avr, void * param)
{
	struct mcu_t * mcu = (struct mcu_t *)param;
	(void)avr;
	avr_rstctrl_request_reset(&mcu->rstctrl, AVR_RSTCTRL_BORF);
}

static void
tinyx1_wdt_reset(struct avr_t * avr, void * param)
{
	struct mcu_t * mcu = (struct mcu_t *)param;
	(void)avr;
	avr_rstctrl_request_reset(&mcu->rstctrl, AVR_RSTCTRL_WDRF);
}

static void
tinyx1_init(struct avr_t * avr)
{
	struct mcu_t * mcu = (struct mcu_t *)avr;

	/* CLKCTRL at 0x60; OSC20M base from FUSE.OSCCFG (fuse index 2). */
	avr_clkctrl_init(avr, &mcu->clkctrl, 0x0060, 2);

	/* NVMCTRL at 0x1000; EEPROM mapped at EEPROM_START. */
	avr_nvmctrl_init(avr, &mcu->nvmctrl, 0x1000, EEPROM_START, SIM_EE_SIZE,
					 NVMCTRL_EE_vect_num);
	/* Flash self-programming, mapped into data space at MAPPED_PROGMEM_START. */
	avr_nvmctrl_set_flash(&mcu->nvmctrl, MAPPED_PROGMEM_START,
						  FLASHEND + 1, PROGMEM_PAGE_SIZE);

	/* PORTA at 0x400 (+ VPORTA at 0x00); PORTB/PORTC fitted by pin count. */
	avr_port_modern_init(avr, &mcu->porta, 'A', 0x0400, 0x0000,
						 PORTA_PORT_vect_num);
#ifdef PORTB_PORT_vect_num
	avr_port_modern_init(avr, &mcu->portb, 'B', 0x0420, 0x0004,
						 PORTB_PORT_vect_num);
#endif
#ifdef PORTC_PORT_vect_num
	avr_port_modern_init(avr, &mcu->portc, 'C', 0x0440, 0x0008,
						 PORTC_PORT_vect_num);
#endif

	/* TCA0 at 0xA00 (single mode): OVF + CMP0/1/2. */
	avr_tca_init(avr, &mcu->tca0, 0x0a00, TCA0_OVF_vect_num,
				 TCA0_CMP0_vect_num, TCA0_CMP1_vect_num, TCA0_CMP2_vect_num, '0');

	/* TCB0 at 0xA40; TCB1 at 0xA50 on the larger parts. */
	avr_tcb_init(avr, &mcu->tcb0, 0x0a40, TCB0_INT_vect_num, '0');
#ifdef TCB1_INT_vect_num
	avr_tcb_init(avr, &mcu->tcb1, 0x0a50, TCB1_INT_vect_num, '1');
#endif

	/* USART0 at 0x0800: RXC/DRE/TXC vectors. */
	avr_usart_modern_init(avr, &mcu->usart0, 0x0800,
						  USART0_RXC_vect_num, USART0_DRE_vect_num,
						  USART0_TXC_vect_num, '0');

	/* TWI0 register block at 0x0810; host/client vectors from the DFP header. */
	avr_twi_modern_init(avr, &mcu->twi, 0x0810,
						TWI0_TWIM_vect_num, TWI0_TWIS_vect_num, '0');

	/* RTC + PIT at 0x0140: RTC_CNT (OVF/CMP) and RTC_PIT vectors. */
	avr_rtc_init(avr, &mcu->rtc, 0x0140,
				 RTC_CNT_vect_num, RTC_PIT_vect_num, '0');

	/* ADC0 at 0x0600: RESRDY + WCOMP vectors; temp sensor cal from SIGROW. */
	avr_adc_modern_init(avr, &mcu->adc0, 0x0600,
						ADC0_RESRDY_vect_num, ADC0_WCOMP_vect_num, '0');
	avr_adc_modern_set_tempsense(&mcu->adc0, SIGROW_TEMPSENSE0);
#ifdef ADC1_RESRDY_vect_num
	/* ADC1 at 0x0640 (16K/32K parts). */
	avr_adc_modern_init(avr, &mcu->adc1, 0x0640,
						ADC1_RESRDY_vect_num, ADC1_WCOMP_vect_num, '1');
#endif

	/* SPI0 at 0x0820: single SPI0_INT vector (normal mode). */
	avr_spi_modern_init(avr, &mcu->spi0, 0x0820, SPI0_INT_vect_num, '0');

	/* AC0 at 0x0680; AC1/AC2 at 0x0688/0x0690 on the larger parts. */
	avr_ac_init(avr, &mcu->ac0, 0x0680, AC0_AC_vect_num, '0');
#ifdef AC1_AC_vect_num
	avr_ac_init(avr, &mcu->ac1, 0x0688, AC1_AC_vect_num, '1');
#endif
#ifdef AC2_AC_vect_num
	avr_ac_init(avr, &mcu->ac2, 0x0690, AC2_AC_vect_num, '2');
#endif

	/* DAC0 (8-bit) at 0x06A0; output feeds AC0's DAC input and ADC0's DAC0
	 * internal channel (MUXPOS 0x1C). */
	avr_dac_init(avr, &mcu->dac0, 0x06a0, '0');
	avr_irq_register_notify(
			avr_io_getirq(avr, AVR_IOCTL_DAC_GETIRQ('0'), AVR_DAC_IRQ_OUT),
			tinyx1_dac_to_ac, &mcu->ac0);
	avr_connect_irq(
			avr_io_getirq(avr, AVR_IOCTL_DAC_GETIRQ('0'), AVR_DAC_IRQ_OUT),
			avr_io_getirq(avr, AVR_IOCTL_ADCM_GETIRQ('0'), AVR_ADCM_CH_DAC0));

	/* CCL (configurable custom logic) at 0x01C0; 2 LUTs on the 1-series. */
	avr_ccl_init(avr, &mcu->ccl, 0x01c0, 2, '0');

	/* Feed live peripheral outputs into the CCL input-source MUX so a LUT
	 * selecting them via INSEL follows the real signal. AC OUT levels map to
	 * the tinyAVR-1 INSEL values 0x6/0xC/0xE (DS40002205A p.413); fewer-AC
	 * parts simply omit the unfitted instances. */
	avr_connect_irq(
			avr_io_getirq(avr, AVR_IOCTL_AC_GETIRQ('0'), AVR_AC_IRQ_OUT),
			avr_io_getirq(avr, AVR_IOCTL_CCL_GETIRQ('0'),
						  AVR_CCL_IRQ_SRC_2LUT(AVR_CCL_SRC_AC0)));
#ifdef AC1_AC_vect_num
	avr_connect_irq(
			avr_io_getirq(avr, AVR_IOCTL_AC_GETIRQ('1'), AVR_AC_IRQ_OUT),
			avr_io_getirq(avr, AVR_IOCTL_CCL_GETIRQ('0'),
						  AVR_CCL_IRQ_SRC_2LUT(AVR_CCL_SRC_AC1)));
#endif
#ifdef AC2_AC_vect_num
	avr_connect_irq(
			avr_io_getirq(avr, AVR_IOCTL_AC_GETIRQ('2'), AVR_AC_IRQ_OUT),
			avr_io_getirq(avr, AVR_IOCTL_CCL_GETIRQ('0'),
						  AVR_CCL_IRQ_SRC_2LUT(AVR_CCL_SRC_AC2)));
#endif

	/* EVSYS (event system) routing fabric at 0x0180. */
	avr_evsys_init(avr, &mcu->evsys, 0x0180, '0');
	avr_irq_register_notify(
			avr_io_getirq(avr, AVR_IOCTL_AC_GETIRQ('0'), AVR_AC_IRQ_OUT),
			tinyx1_ac0_to_evsys, mcu);
	avr_irq_register_notify(
			avr_io_getirq(avr, AVR_IOCTL_EVSYS_GETIRQ('0'),
						  AVR_EVSYS_IRQ_USER0 + AVR_EVSYS_USER_ADC0),
			tinyx1_evsys_to_adc, mcu);
	avr_irq_register_notify(
			avr_io_getirq(avr, AVR_IOCTL_TCB_GETIRQ('0'), AVR_TCB_IRQ_CAPT_OUT),
			tinyx1_tcb0_capt_to_evsys, mcu);
	avr_irq_register_notify(
			avr_io_getirq(avr, AVR_IOCTL_EVSYS_GETIRQ('0'),
						  AVR_EVSYS_IRQ_USER0 + AVR_EVSYS_USER_TCB0),
			tinyx1_evsys_to_tcb0, mcu);
#ifdef TCB1_INT_vect_num
	avr_irq_register_notify(
			avr_io_getirq(avr, AVR_IOCTL_TCB_GETIRQ('1'), AVR_TCB_IRQ_CAPT_OUT),
			tinyx1_tcb1_capt_to_evsys, mcu);
	avr_irq_register_notify(
			avr_io_getirq(avr, AVR_IOCTL_EVSYS_GETIRQ('0'),
						  AVR_EVSYS_IRQ_USER0 + 11 /* ASYNCUSER11 = TCB1 */),
			tinyx1_evsys_to_tcb1, mcu);
#endif
	/* EVSYS SYNCUSER0 = TCA0 event input (EVCTRL.CNTEI counting / clock
	 * gating); the routed channel level drives TCA0's EV_IN. */
	avr_connect_irq(
			avr_io_getirq(avr, AVR_IOCTL_EVSYS_GETIRQ('0'),
						  AVR_EVSYS_IRQ_USER0 + AVR_EVSYS_USER_TCA0),
			avr_io_getirq(avr, AVR_IOCTL_TCA_GETIRQ('0'), AVR_TCA_IRQ_EV_IN));

	/* PORTMUX (peripheral pin routing) config store at 0x0200. */
	avr_portmux_init(avr, &mcu->portmux, 0x0200, '0');

	/* VREF (voltage reference selection) at 0x00A0; DAC0REFSEL feeds DAC0/AC0. */
	avr_vref_init(avr, &mcu->vref, 0x00a0, '0');
	avr_irq_register_notify(
			avr_io_getirq(avr, AVR_IOCTL_VREF_GETIRQ('0'), AVR_VREF_IRQ_DAC0_MV),
			tinyx1_vref_to_dac_ac, mcu);
	avr_irq_register_notify(
			avr_io_getirq(avr, AVR_IOCTL_VREF_GETIRQ('0'), AVR_VREF_IRQ_ADC0_MV),
			tinyx1_vref_to_adc, mcu);
#ifdef ADC1_RESRDY_vect_num
	/* CTRLC.ADC1REFSEL -> ADC1 (16K/32K parts). */
	avr_irq_register_notify(
			avr_io_getirq(avr, AVR_IOCTL_VREF_GETIRQ('0'), AVR_VREF_IRQ_ADC1_MV),
			tinyx1_vref_to_adc1, mcu);
#endif
#ifdef AC1_AC_vect_num
	/* CTRLC.DAC1REFSEL -> AC1 reference (DAC1 absent). */
	avr_irq_register_notify(
			avr_io_getirq(avr, AVR_IOCTL_VREF_GETIRQ('0'), AVR_VREF_IRQ_DAC1_MV),
			tinyx1_vref_to_ac1, mcu);
#endif
#ifdef AC2_AC_vect_num
	/* CTRLD.DAC2REFSEL -> AC2 reference (DAC2 absent). */
	avr_irq_register_notify(
			avr_io_getirq(avr, AVR_IOCTL_VREF_GETIRQ('0'), AVR_VREF_IRQ_DAC2_MV),
			tinyx1_vref_to_ac2, mcu);
#endif

	/* TCD0 (12-bit timer type D) at 0x0A80: periodic OVF vector.
	 * OSCCFG fuse index 2 resolves the OSC20M base for CLKSEL=OSC20M. */
	avr_tcd_init(avr, &mcu->tcd0, 0x0a80, TCD0_OVF_vect_num, 2, '0');

	/* TCD0 WOA/WOB feed CCL INSEL 0x9 (DS40002205A p.413-415: IN0/IN2->WOA,
	 * IN1->WOB). */
	avr_connect_irq(
			avr_io_getirq(avr, AVR_IOCTL_TCD_GETIRQ('0'), AVR_TCD_IRQ_WOA),
			avr_io_getirq(avr, AVR_IOCTL_CCL_GETIRQ('0'),
						  AVR_CCL_IRQ_SRC_2LUT(AVR_CCL_SRC_TCD0_WOA)));
	avr_connect_irq(
			avr_io_getirq(avr, AVR_IOCTL_TCD_GETIRQ('0'), AVR_TCD_IRQ_WOB),
			avr_io_getirq(avr, AVR_IOCTL_CCL_GETIRQ('0'),
						  AVR_CCL_IRQ_SRC_2LUT(AVR_CCL_SRC_TCD0_WOB)));

	/* TCA0 WO0/WO1/WO2 feed CCL INSEL 0x8 (DS40002205A p.413-415: WOn on
	 * INn), so a single-slope PWM channel can drive a LUT directly. */
	for (int wo = 0; wo < 3; wo++)
		avr_connect_irq(
				avr_io_getirq(avr, AVR_IOCTL_TCA_GETIRQ('0'), AVR_TCA_IRQ_WO0 + wo),
				avr_io_getirq(avr, AVR_IOCTL_CCL_GETIRQ('0'),
							  AVR_CCL_IRQ_SRC_2LUT(AVR_CCL_SRC_TCA0_WO0 + wo)));

	/* TCB0 WO feeds CCL INSEL 0x7 (DS40002205A p.413: "TCB0 WO input source");
	 * TCB1 WO feeds INSEL 0xD on the larger parts. The TCB emits its 8-bit-PWM
	 * waveform level on this IRQ. */
	avr_connect_irq(
			avr_io_getirq(avr, AVR_IOCTL_TCB_GETIRQ('0'), AVR_TCB_IRQ_WO),
			avr_io_getirq(avr, AVR_IOCTL_CCL_GETIRQ('0'),
						  AVR_CCL_IRQ_SRC_2LUT(AVR_CCL_SRC_TCB0)));
#ifdef TCB1_INT_vect_num
	avr_connect_irq(
			avr_io_getirq(avr, AVR_IOCTL_TCB_GETIRQ('1'), AVR_TCB_IRQ_WO),
			avr_io_getirq(avr, AVR_IOCTL_CCL_GETIRQ('0'),
						  AVR_CCL_IRQ_SRC_2LUT(AVR_CCL_SRC_TCB1)));
#endif

	/* WDT (modern reset-only watchdog) at 0x0100. */
	avr_wdt_modern_init(avr, &mcu->wdt, 0x0100, '0');
	avr_wdt_modern_set_reset_handler(&mcu->wdt, tinyx1_wdt_reset, mcu);

	/* CRCSCAN (flash CRC memory scan) at 0x0120; APPEND/BOOTEND fuses (indices
	 * 7/8) bound the sections, CRC failure raises the NMI. */
	avr_crcscan_init(avr, &mcu->crcscan, 0x0120, FLASHEND + 1, 5, 7, 8,
					 CRCSCAN_NMI_vect_num, '0');

	/* SLPCTRL (sleep controller) at 0x0050. */
	avr_slpctrl_init(avr, &mcu->slpctrl, 0x0050, '0');

	/* CPUINT (interrupt controller registers) at 0x0110. */
	avr_cpuint_init(avr, &mcu->cpuint, 0x0110, '0');

	/* RSTCTRL (reset controller) at 0x0040. */
	avr_rstctrl_init(avr, &mcu->rstctrl, 0x0040, '0');

	/* BOD (brown-out detector / VLM) at 0x0080; CTRLA/B from FUSE.BODCFG
	 * (fuse index 1); BOD_VLM interrupt vector. */
	avr_bod_init(avr, &mcu->bod, 0x0080, BOD_VLM_vect_num, 1, '0');
	avr_bod_set_brownout_handler(&mcu->bod, tinyx1_bod_brownout, mcu);

	/* SYSCFG (REVID/EXTBRK) at 0x0F00 and the signature row (SIGROW) at 0x1100;
	 * DEVICEID is the device signature, revision A (0x00). */
	avr_syscfg_init(avr, &mcu->syscfg, 0x0f00, 0x1100, 0x00);
}

static void
tinyx1_reset(struct avr_t * avr)
{
	(void)avr;
}

const struct mcu_t SIM_CORENAME = {
	.core = {
		.mmcu = SIM_MMCU,
		MODERN_CORE(SIM_VECTOR_SIZE),

		.init = tinyx1_init,
		.reset = tinyx1_reset,
	},
};

/*
 * The kind descriptor for each part is declared in its own cores/sim_tinyNNN.c
 * (with a literal declaration the build's core-table generator can grep for),
 * using SIM_CORENAME and this make helper. (The token the generator scans for is
 * deliberately not spelled out in this shared header.)
 */
static avr_t *
sim_tinyx1_make(void)
{
	return avr_core_allocate(&SIM_CORENAME.core, sizeof(struct mcu_t));
}
