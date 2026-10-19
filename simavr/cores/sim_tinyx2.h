/*
	sim_tinyx2.h

	Shared core template for the Microchip tinyAVR(R) 2-series (AVRxt):
	ATtiny424/426/427, 824/826/827, 1624/1626/1627 and 3224/3226/3227. Every
	2-series part has the same peripheral architecture and register map; they
	differ in memory sizes, signature and pin count. The 14-pin x24 parts fit
	PORTA and PORTB[3:0] only; the x26/x27 parts add PORTC, which a core opts
	into with SIM_HAS_PORTC (the interrupt vector table is shared by the whole
	family, so it cannot tell the pin counts apart — DS40002311B Table 2).
	Memory sizes, signature and vector numbers come straight from the device's
	own avr-libc header, which is generated from the same Microchip device files
	as the datasheet.

	The 2-series is *not* a 1-series with new addresses: it has a 12-bit
	differential ADC with PGA, a channel/user EVSYS with six channels, four CCL
	LUTs with an interrupt, a VREF that only selects the AC0 reference (the ADC
	picks its own), AC0 with DACREF, two USARTs, no DAC and no TCD0. Peripheral
	bases also move (TWI0 0x8A0, SPI0 0x8C0, TCB0 0xA80, PORTMUX 0x5E0). Each
	difference below was checked against DS40002311B (ATtiny424..827); the
	1624..1627 (DS40002234B) and 3224..3227 (DS40002345B) datasheets describe the
	same peripherals.

	A concrete core (cores/sim_tinyNNN.c) is then just:

		#include "sim_avr.h"
		#define SIM_MMCU     "attinyNNN"
		#define SIM_CORENAME mcu_tinyNNN
		#define SIM_HAS_PORTC 1             // x26 / x27 parts only
		#define _AVR_IO_H_
		#define __ASSEMBLER__
		#define _SFR_MEM8(x)  (x)
		#define _SFR_MEM16(x) (x)
		#define _SFR_IO8(x)   (x)
		#include "avr/iotnNNN.h"
		#include "sim_tinyx2.h"

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

/* See sim_tinyx1.h: drop the device header's identity macros before
 * sim_core_declare.h defines its own. Only plain integer macros are used. */
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
#include "avr_adc_tiny2.h"
#include "avr_spi_modern.h"
#include "avr_ac.h"
#include "avr_ccl.h"
#include "avr_evsys.h"
#include "avr_portmux.h"
#include "avr_vref.h"
#include "avr_wdt.h"
#include "avr_crcscan.h"
#include "avr_slpctrl.h"
#include "avr_cpuint.h"
#include "avr_rstctrl.h"
#include "avr_bod.h"
#include "avr_syscfg.h"

/*
 * The device structure: PORTA/PORTB, TCA0, TCB0/1, USART0/1, TWI0, SPI0, RTC,
 * ADC0, AC0 and the system peripherals are on every part; PORTC only when the
 * core defines SIM_HAS_PORTC.
 */
struct mcu_t {
	avr_t				core;
	avr_clkctrl_t		clkctrl;
	avr_nvmctrl_t		nvmctrl;
	avr_port_modern_t	porta;
	avr_port_modern_t	portb;
#ifdef SIM_HAS_PORTC
	avr_port_modern_t	portc;
#endif
	avr_tca_t			tca0;
	avr_tcb_t			tcb0;
	avr_tcb_t			tcb1;
	avr_usart_modern_t	usart0;
	avr_usart_modern_t	usart1;
	avr_twi_modern_t	twi;
	avr_rtc_t			rtc;
	avr_adc_tiny2_t		adc0;
	avr_spi_modern_t	spi0;
	avr_ac_t			ac0;
	avr_ccl_t			ccl;
	avr_evsys_t			evsys;
	avr_portmux_t		portmux;
	avr_vref_t			vref;
	avr_wdt_modern_t	wdt;
	avr_crcscan_t		crcscan;
	avr_slpctrl_t		slpctrl;
	avr_cpuint_t		cpuint;
	avr_rstctrl_t		rstctrl;
	avr_bod_t			bod;
	avr_syscfg_t		syscfg;
};

/* VREF.CTRLA.AC0REFSEL -> the AC0 reference (the ADC selects its own). */
static void
tinyx2_vref_to_ac(struct avr_irq_t * irq, uint32_t value, void * param)
{
	struct mcu_t * mcu = (struct mcu_t *)param;
	(void)irq;
	avr_ac_set_refs(&mcu->ac0, value, 0);
}

/*
 * EVSYS generator values (CHANNELn, DS40002311B 15.5.2) and user indices
 * (USERxxx at 0x20 + n, 15.5.3 / iotn3224.h EVSYS_t).
 */
#define TINYX2_GEN_CCL_LUT0		0x10	/* LUT0..3 = 0x10..0x13 */
#define TINYX2_GEN_AC0_OUT		0x20
#define TINYX2_GEN_ADC0_RES		0x24
#define TINYX2_GEN_ADC0_SAMP		0x25
#define TINYX2_GEN_ADC0_WCMP		0x26
#define TINYX2_GEN_TCB0_CAPT		0xa0
#define TINYX2_GEN_TCB1_CAPT		0xa2
#define TINYX2_USER_ADC0START		8	/* USERADC0START   @ 0x28 */
#define TINYX2_USER_TCA0CNTA		14	/* USERTCA0CNTA    @ 0x2E */
#define TINYX2_USER_TCB0CAPT		16	/* USERTCB0CAPT    @ 0x30 */
#define TINYX2_USER_TCB1CAPT		18	/* USERTCB1CAPT    @ 0x32 */

static void
tinyx2_gen_to_evsys(struct avr_irq_t * irq, uint32_t value, void * param,
					uint8_t gen)
{
	struct mcu_t * mcu = (struct mcu_t *)param;
	(void)irq;
	avr_evsys_async_generator(&mcu->evsys, gen, value & 1);
}

static void
tinyx2_ac0_to_evsys(struct avr_irq_t * irq, uint32_t value, void * param)
{
	tinyx2_gen_to_evsys(irq, value, param, TINYX2_GEN_AC0_OUT);
}

static void
tinyx2_adc_res_to_evsys(struct avr_irq_t * irq, uint32_t value, void * param)
{
	tinyx2_gen_to_evsys(irq, value, param, TINYX2_GEN_ADC0_RES);
}

static void
tinyx2_adc_samp_to_evsys(struct avr_irq_t * irq, uint32_t value, void * param)
{
	tinyx2_gen_to_evsys(irq, value, param, TINYX2_GEN_ADC0_SAMP);
}

static void
tinyx2_adc_wcmp_to_evsys(struct avr_irq_t * irq, uint32_t value, void * param)
{
	tinyx2_gen_to_evsys(irq, value, param, TINYX2_GEN_ADC0_WCMP);
}

static void
tinyx2_tcb0_capt_to_evsys(struct avr_irq_t * irq, uint32_t value, void * param)
{
	tinyx2_gen_to_evsys(irq, value, param, TINYX2_GEN_TCB0_CAPT);
}

static void
tinyx2_tcb1_capt_to_evsys(struct avr_irq_t * irq, uint32_t value, void * param)
{
	tinyx2_gen_to_evsys(irq, value, param, TINYX2_GEN_TCB1_CAPT);
}

/* CCL LUTn output -> EVSYS generator 0x10 + n. The IRQ index of LUT n's
 * output is stable across the 4-LUT block, so one notifier per LUT. */
static void
tinyx2_ccl_lut_to_evsys(struct avr_irq_t * irq, uint32_t value, void * param)
{
	struct mcu_t * mcu = (struct mcu_t *)param;
	int lut = irq->irq - (mcu->ccl.base_irq + AVR_CCL_LUT_INPUTS * 4);
	if (lut >= 0 && lut < 4)
		avr_evsys_async_generator(&mcu->evsys, TINYX2_GEN_CCL_LUT0 + lut,
								  value & 1);
}

/* EVSYS ADC0START user: a rising event edge starts a conversion (honoured when
 * COMMAND.START = EVENT_TRIGGER). */
static void
tinyx2_evsys_to_adc(struct avr_irq_t * irq, uint32_t value, void * param)
{
	struct mcu_t * mcu = (struct mcu_t *)param;
	(void)irq;
	if (value & 1)
		avr_adc_tiny2_event_start(&mcu->adc0);
}

static void
tinyx2_evsys_to_tcb0(struct avr_irq_t * irq, uint32_t value, void * param)
{
	struct mcu_t * mcu = (struct mcu_t *)param;
	(void)irq;
	avr_raise_irq(avr_io_getirq(&mcu->core, AVR_IOCTL_TCB_GETIRQ('0'),
								AVR_TCB_IRQ_EVENT_IN), value & 1);
}

static void
tinyx2_evsys_to_tcb1(struct avr_irq_t * irq, uint32_t value, void * param)
{
	struct mcu_t * mcu = (struct mcu_t *)param;
	(void)irq;
	avr_raise_irq(avr_io_getirq(&mcu->core, AVR_IOCTL_TCB_GETIRQ('1'),
								AVR_TCB_IRQ_EVENT_IN), value & 1);
}

/*
 * On-chip routing: a BOD brown-out resets the device through RSTCTRL, which
 * records the cause in RSTFR.BORF; likewise a watchdog time-out (WDRF).
 */
static void
tinyx2_bod_brownout(struct avr_t * avr, void * param)
{
	struct mcu_t * mcu = (struct mcu_t *)param;
	(void)avr;
	avr_rstctrl_request_reset(&mcu->rstctrl, AVR_RSTCTRL_BORF);
}

static void
tinyx2_wdt_reset(struct avr_t * avr, void * param)
{
	struct mcu_t * mcu = (struct mcu_t *)param;
	(void)avr;
	avr_rstctrl_request_reset(&mcu->rstctrl, AVR_RSTCTRL_WDRF);
}

static void
tinyx2_init(struct avr_t * avr)
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
	/* USERROW (32 bytes) at USER_SIGNATURES_START (0x1300). */
	avr_nvmctrl_set_userrow(&mcu->nvmctrl, USER_SIGNATURES_START,
							USER_SIGNATURES_SIZE);

	/* PORTA/PORTB (+ VPORTs); PORTC on the x26/x27 parts. */
	avr_port_modern_init(avr, &mcu->porta, 'A', 0x0400, 0x0000,
						 PORTA_PORT_vect_num);
	avr_port_modern_init(avr, &mcu->portb, 'B', 0x0420, 0x0004,
						 PORTB_PORT_vect_num);
#ifdef SIM_HAS_PORTC
	avr_port_modern_init(avr, &mcu->portc, 'C', 0x0440, 0x0008,
						 PORTC_PORT_vect_num);
#endif

	/* TCA0 at 0xA00 (single mode): OVF + CMP0/1/2. */
	avr_tca_init(avr, &mcu->tca0, 0x0a00, TCA0_OVF_vect_num,
				 TCA0_CMP0_vect_num, TCA0_CMP1_vect_num, TCA0_CMP2_vect_num, '0');

	/* TCB0 at 0xA80, TCB1 at 0xA90. */
	avr_tcb_init(avr, &mcu->tcb0, 0x0a80, TCB0_INT_vect_num, '0');
	avr_tcb_init(avr, &mcu->tcb1, 0x0a90, TCB1_INT_vect_num, '1');

	/* USART0 at 0x800, USART1 at 0x820. */
	avr_usart_modern_init(avr, &mcu->usart0, 0x0800,
						  USART0_RXC_vect_num, USART0_DRE_vect_num,
						  USART0_TXC_vect_num, '0');
	avr_usart_modern_init(avr, &mcu->usart1, 0x0820,
						  USART1_RXC_vect_num, USART1_DRE_vect_num,
						  USART1_TXC_vect_num, '1');

	/* TWI0 at 0x8A0, SPI0 at 0x8C0 (neither is at its 1-series address). */
	avr_twi_modern_init(avr, &mcu->twi, 0x08a0,
						TWI0_TWIM_vect_num, TWI0_TWIS_vect_num, '0');
	avr_spi_modern_init(avr, &mcu->spi0, 0x08c0, SPI0_INT_vect_num, '0');

	/* RTC + PIT at 0x0140. */
	avr_rtc_init(avr, &mcu->rtc, 0x0140,
				 RTC_CNT_vect_num, RTC_PIT_vect_num, '0');

	/* ADC0 at 0x0600 (12-bit differential, PGA): ERROR, RESRDY and SAMPRDY
	 * vectors; temperature calibration from SIGROW.TEMPSENSE0/1. Its DACREF0
	 * input follows AC0's DACREF output. */
	avr_adc_tiny2_init(avr, &mcu->adc0, 0x0600, ADC0_ERROR_vect_num,
					   ADC0_RESRDY_vect_num, ADC0_SAMPRDY_vect_num, '0');
	avr_adc_tiny2_set_tempsense(&mcu->adc0, SIGROW_TEMPSENSE0);

	/* AC0 at 0x0680 with DACREF; its reference is VREF.CTRLA.AC0REFSEL. */
	avr_ac_init_dacref(avr, &mcu->ac0, 0x0680, AC0_AC_vect_num, '0');
	avr_adc_tiny2_set_dacref_source(&mcu->adc0, &mcu->ac0.dacref_mv);

	/* CCL at 0x01C0: 4 LUTs, megaAVR-style layout, one CCL_CCL interrupt. */
	avr_ccl_init_mega(avr, &mcu->ccl, 0x01c0, 4, '0');
	avr_ccl_set_interrupt(avr, &mcu->ccl, CCL_CCL_vect_num);

	/* Live peripheral outputs into the CCL input-source MUX (DS40002311B
	 * 28.2.2.1): AC0 OUT = INSEL 0x6, TCA0 WO0-2 = 0xA, TCB0/TCB1 WO = 0xC
	 * (IN0/IN1). USART TXD (0x8) and SPI (0x9) need line-level output IRQs
	 * that those models do not expose yet. */
	avr_connect_irq(
			avr_io_getirq(avr, AVR_IOCTL_AC_GETIRQ('0'), AVR_AC_IRQ_OUT),
			avr_io_getirq(avr, AVR_IOCTL_CCL_GETIRQ('0'),
						  AVR_CCL_IRQ_SRC_4LUT(AVR_CCL_SRC_AC0)));
	for (int wo = 0; wo < 3; wo++)
		avr_connect_irq(
				avr_io_getirq(avr, AVR_IOCTL_TCA_GETIRQ('0'), AVR_TCA_IRQ_WO0 + wo),
				avr_io_getirq(avr, AVR_IOCTL_CCL_GETIRQ('0'),
							  AVR_CCL_IRQ_SRC_4LUT(AVR_CCL_SRC_TCA0_WO0 + wo)));
	avr_connect_irq(
			avr_io_getirq(avr, AVR_IOCTL_TCB_GETIRQ('0'), AVR_TCB_IRQ_WO),
			avr_io_getirq(avr, AVR_IOCTL_CCL_GETIRQ('0'),
						  AVR_CCL_IRQ_SRC_4LUT(AVR_CCL_SRC_TCB0)));
	avr_connect_irq(
			avr_io_getirq(avr, AVR_IOCTL_TCB_GETIRQ('1'), AVR_TCB_IRQ_WO),
			avr_io_getirq(avr, AVR_IOCTL_CCL_GETIRQ('0'),
						  AVR_CCL_IRQ_SRC_4LUT(AVR_CCL_SRC_TCB1)));

	/* EVSYS at 0x0180: six channels, twenty users. */
	avr_evsys_init_xt(avr, &mcu->evsys, 0x0180, '0', 6, 20);

	/* Generators: AC0 OUT, ADC0 RES/SAMP/WCMP, TCB0/1 CAPT, CCL LUT0-3. */
	avr_irq_register_notify(
			avr_io_getirq(avr, AVR_IOCTL_AC_GETIRQ('0'), AVR_AC_IRQ_OUT),
			tinyx2_ac0_to_evsys, mcu);
	avr_irq_register_notify(
			avr_io_getirq(avr, AVR_IOCTL_ADC2_GETIRQ('0'), AVR_ADC2_IRQ_EVT_RES),
			tinyx2_adc_res_to_evsys, mcu);
	avr_irq_register_notify(
			avr_io_getirq(avr, AVR_IOCTL_ADC2_GETIRQ('0'), AVR_ADC2_IRQ_EVT_SAMP),
			tinyx2_adc_samp_to_evsys, mcu);
	avr_irq_register_notify(
			avr_io_getirq(avr, AVR_IOCTL_ADC2_GETIRQ('0'), AVR_ADC2_IRQ_EVT_WCMP),
			tinyx2_adc_wcmp_to_evsys, mcu);
	avr_irq_register_notify(
			avr_io_getirq(avr, AVR_IOCTL_TCB_GETIRQ('0'), AVR_TCB_IRQ_CAPT_OUT),
			tinyx2_tcb0_capt_to_evsys, mcu);
	avr_irq_register_notify(
			avr_io_getirq(avr, AVR_IOCTL_TCB_GETIRQ('1'), AVR_TCB_IRQ_CAPT_OUT),
			tinyx2_tcb1_capt_to_evsys, mcu);
	for (int lut = 0; lut < 4; lut++)
		avr_irq_register_notify(
				avr_io_getirq(avr, AVR_IOCTL_CCL_GETIRQ('0'),
							  AVR_CCL_LUT_INPUTS * 4 + lut),
				tinyx2_ccl_lut_to_evsys, mcu);

	/* Users: ADC0 START, TCB0/TCB1 CAPT, TCA0 CNTA. (The CCL LUT event inputs
	 * and the TCB COUNT users are not modelled.) */
	avr_irq_register_notify(
			avr_io_getirq(avr, AVR_IOCTL_EVSYS_GETIRQ('0'),
						  AVR_EVSYS_IRQ_USER0 + TINYX2_USER_ADC0START),
			tinyx2_evsys_to_adc, mcu);
	avr_irq_register_notify(
			avr_io_getirq(avr, AVR_IOCTL_EVSYS_GETIRQ('0'),
						  AVR_EVSYS_IRQ_USER0 + TINYX2_USER_TCB0CAPT),
			tinyx2_evsys_to_tcb0, mcu);
	avr_irq_register_notify(
			avr_io_getirq(avr, AVR_IOCTL_EVSYS_GETIRQ('0'),
						  AVR_EVSYS_IRQ_USER0 + TINYX2_USER_TCB1CAPT),
			tinyx2_evsys_to_tcb1, mcu);
	avr_connect_irq(
			avr_io_getirq(avr, AVR_IOCTL_EVSYS_GETIRQ('0'),
						  AVR_EVSYS_IRQ_USER0 + TINYX2_USER_TCA0CNTA),
			avr_io_getirq(avr, AVR_IOCTL_TCA_GETIRQ('0'), AVR_TCA_IRQ_EV_IN));

	/* PORTMUX at 0x05E0: six *ROUTEA registers (EVSYS, CCL, USART, SPI, TCA, TCB). */
	avr_portmux_init_n(avr, &mcu->portmux, 0x05e0, 6, '0');

	/* VREF at 0x00A0: AC0REFSEL feeds AC0. */
	avr_vref_init_tiny2(avr, &mcu->vref, 0x00a0, '0');
	avr_irq_register_notify(
			avr_io_getirq(avr, AVR_IOCTL_VREF_GETIRQ('0'), AVR_VREF_IRQ_AC0_MV),
			tinyx2_vref_to_ac, mcu);

	/* WDT (modern reset-only watchdog) at 0x0100. */
	avr_wdt_modern_init(avr, &mcu->wdt, 0x0100, '0');
	avr_wdt_modern_set_reset_handler(&mcu->wdt, tinyx2_wdt_reset, mcu);

	/* CRCSCAN at 0x0120; SYSCFG0 (5) / APPEND (7) / BOOTEND (8) fuses bound the
	 * sections, CRC failure raises the NMI. */
	avr_crcscan_init(avr, &mcu->crcscan, 0x0120, FLASHEND + 1, 5, 7, 8,
					 CRCSCAN_NMI_vect_num, '0');

	/* SLPCTRL at 0x0050. */
	avr_slpctrl_init(avr, &mcu->slpctrl, 0x0050, '0');

	/* CPUINT at 0x0110; FUSE.BOOTEND (index 8) sets the IVSEL relocation base. */
	avr_cpuint_init(avr, &mcu->cpuint, 0x0110, 8, '0');

	/* RSTCTRL at 0x0040. */
	avr_rstctrl_init(avr, &mcu->rstctrl, 0x0040, '0');

	/* BOD / VLM at 0x0080; CTRLA/B from FUSE.BODCFG (fuse index 1). */
	avr_bod_init(avr, &mcu->bod, 0x0080, BOD_VLM_vect_num, 1, '0');
	avr_bod_set_brownout_handler(&mcu->bod, tinyx2_bod_brownout, mcu);

	/* SYSCFG 0x0F00 (REVID), SIGROW 0x1100, FUSE window 0x1280 (9 bytes =
	 * FUSE_t); revision A. */
	avr_syscfg_init(avr, &mcu->syscfg, 0x0f00, 0x1100, 0x1280, 9, 0x00);
}

static void
tinyx2_reset(struct avr_t * avr)
{
	(void)avr;
}

const struct mcu_t SIM_CORENAME = {
	.core = {
		.mmcu = SIM_MMCU,
		MODERN_CORE(SIM_VECTOR_SIZE),

		.init = tinyx2_init,
		.reset = tinyx2_reset,
	},
};

/*
 * The kind descriptor for each part is declared in its own cores/sim_tinyNNN.c
 * (with a literal declaration the build's core-table generator can grep for),
 * using SIM_CORENAME and this make helper.
 */
static avr_t *
sim_tinyx2_make(void)
{
	return avr_core_allocate(&SIM_CORENAME.core, sizeof(struct mcu_t));
}
