/*
	sim_tiny3217.c

	ATtiny3217 (tinyAVR 1-series, AVRxt core) — first "modern" AVR core in
	simavr. This initial descriptor wires up the engine in modern mode and the
	TWI0 (I2C) peripheral; other modern peripherals (PORT/VPORT, TCA/TCB, USART0,
	NVMCTRL, ADC0, …) are added incrementally per doc/attiny3217_design.md.

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

#include "sim_avr.h"

#define SIM_VECTOR_SIZE	4			/* ATtiny3217 emits JMP-based vectors */
#define SIM_MMCU		"attiny3217"
#define SIM_CORENAME	mcu_tiny3217

#define _AVR_IO_H_
#define __ASSEMBLER__
#include "avr/iotn3217.h"

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
#include "avr_rstctrl.h"
#include "avr_bod.h"
#include "avr_syscfg.h"

/*
 * The ATtiny3217 device structure. Grows as peripherals are added; for now it
 * carries the core, CLKCTRL, NVMCTRL, PORTA/B/C (+VPORTs), TCA0, TCB0/1, USART0,
 * TWI0, the RTC (+PIT), ADC0, SPI0, AC0, DAC0, CCL, EVSYS, PORTMUX, VREF, TCD0,
 * the WDT, CRCSCAN, SLPCTRL, RSTCTRL, BOD, and SYSCFG/SIGROW (device identity).
 */
struct mcu_t {
	avr_t				core;
	avr_clkctrl_t		clkctrl;
	avr_nvmctrl_t		nvmctrl;
	avr_port_modern_t	porta, portb, portc;
	avr_tca_t			tca0;
	avr_tcb_t			tcb0, tcb1;
	avr_usart_modern_t	usart0;
	avr_twi_modern_t	twi;
	avr_rtc_t			rtc;
	avr_adc_modern_t	adc0;
	avr_spi_modern_t	spi0;
	avr_ac_t			ac0;
	avr_dac_t			dac0;
	avr_ccl_t			ccl;
	avr_evsys_t			evsys;
	avr_portmux_t		portmux;
	avr_vref_t			vref;
	avr_tcd_t			tcd0;
	avr_wdt_modern_t	wdt;
	avr_crcscan_t		crcscan;
	avr_slpctrl_t		slpctrl;
	avr_rstctrl_t		rstctrl;
	avr_bod_t			bod;
	avr_syscfg_t		syscfg;
};

/*
 * On-chip routing: the DAC output feeds the analog comparator's "DAC" negative
 * input. Mirror that by updating AC0's DAC reference whenever DAC0's output
 * (millivolts) changes.
 */
static void
tiny3217_dac_to_ac(struct avr_irq_t * irq, uint32_t value, void * param)
{
	avr_ac_t * ac = (avr_ac_t *)param;
	(void)irq;
	avr_ac_set_refs(ac, ac->vref_mv, value);
}

/*
 * On-chip routing: VREF.CTRLA.DAC0REFSEL selects the internal reference for
 * both DAC0 and AC0 (their reference is always the internal VREF). Push the
 * decoded reference voltage (millivolts) to both whenever firmware programs it.
 * ADC0's reference additionally depends on ADC.CTRLC.REFSEL (internal vs VDD),
 * which the ADC model does not yet distinguish, so AVR_VREF_IRQ_ADC0_MV is left
 * unwired.
 */
static void
tiny3217_vref_to_dac_ac(struct avr_irq_t * irq, uint32_t value, void * param)
{
	struct mcu_t * mcu = (struct mcu_t *)param;
	(void)irq;
	avr_dac_set_vref(&mcu->dac0, value);
	avr_ac_set_refs(&mcu->ac0, value, mcu->ac0.dacref_mv);
}

static void
tiny3217_init(struct avr_t * avr)
{
	struct mcu_t * mcu = (struct mcu_t *)avr;

	/* CLKCTRL at 0x60; OSC20M base from FUSE.OSCCFG (fuse index 2). */
	avr_clkctrl_init(avr, &mcu->clkctrl, 0x0060, 2);

	/* NVMCTRL at 0x1000; EEPROM mapped at 0x1400 (256 bytes). */
	avr_nvmctrl_init(avr, &mcu->nvmctrl, 0x1000, 0x1400, 256,
					 NVMCTRL_EE_vect_num);
	/* Flash self-programming: 32 KB mapped at 0x8000, 128-byte pages. */
	avr_nvmctrl_set_flash(&mcu->nvmctrl, 0x8000, 32768, 128);

	/* PORTA/B/C at 0x400/0x420/0x440; VPORTA/B/C at 0x00/0x04/0x08. */
	avr_port_modern_init(avr, &mcu->porta, 'A', 0x0400, 0x0000,
						 PORTA_PORT_vect_num);
	avr_port_modern_init(avr, &mcu->portb, 'B', 0x0420, 0x0004,
						 PORTB_PORT_vect_num);
	avr_port_modern_init(avr, &mcu->portc, 'C', 0x0440, 0x0008,
						 PORTC_PORT_vect_num);

	/* TCA0 at 0xA00 (single mode): OVF + CMP0/1/2. */
	avr_tca_init(avr, &mcu->tca0, 0x0a00, TCA0_OVF_vect_num,
				 TCA0_CMP0_vect_num, TCA0_CMP1_vect_num, TCA0_CMP2_vect_num, '0');

	/* TCB0/TCB1 at 0xA40/0xA50. */
	avr_tcb_init(avr, &mcu->tcb0, 0x0a40, TCB0_INT_vect_num, '0');
	avr_tcb_init(avr, &mcu->tcb1, 0x0a50, TCB1_INT_vect_num, '1');

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

	/* ADC0 at 0x0600: RESRDY + WCOMP (window comparator) vectors. */
	avr_adc_modern_init(avr, &mcu->adc0, 0x0600,
						ADC0_RESRDY_vect_num, ADC0_WCOMP_vect_num, '0');

	/* SPI0 at 0x0820: single SPI0_INT vector (normal mode). */
	avr_spi_modern_init(avr, &mcu->spi0, 0x0820, SPI0_INT_vect_num, '0');

	/* AC0 (analog comparator) at 0x0680: AC0_AC vector. */
	avr_ac_init(avr, &mcu->ac0, 0x0680, AC0_AC_vect_num, '0');

	/* DAC0 (8-bit) at 0x06A0; its output feeds AC0's DAC negative input and the
	 * ADC0 internal DAC0 channel (MUXPOS 0x1C). */
	avr_dac_init(avr, &mcu->dac0, 0x06a0, '0');
	avr_irq_register_notify(
			avr_io_getirq(avr, AVR_IOCTL_DAC_GETIRQ('0'), AVR_DAC_IRQ_OUT),
			tiny3217_dac_to_ac, &mcu->ac0);
	avr_connect_irq(
			avr_io_getirq(avr, AVR_IOCTL_DAC_GETIRQ('0'), AVR_DAC_IRQ_OUT),
			avr_io_getirq(avr, AVR_IOCTL_ADCM_GETIRQ('0'), AVR_ADCM_CH_DAC0));

	/* CCL (configurable custom logic) at 0x01C0; 2 LUTs on the ATtiny3217. */
	avr_ccl_init(avr, &mcu->ccl, 0x01c0, 2, '0');

	/* EVSYS (event system) routing fabric at 0x0180. */
	avr_evsys_init(avr, &mcu->evsys, 0x0180, '0');

	/* PORTMUX (peripheral pin routing) config store at 0x0200. */
	avr_portmux_init(avr, &mcu->portmux, 0x0200, '0');

	/* VREF (voltage reference selection) at 0x00A0; DAC0REFSEL feeds DAC0/AC0. */
	avr_vref_init(avr, &mcu->vref, 0x00a0, '0');
	avr_irq_register_notify(
			avr_io_getirq(avr, AVR_IOCTL_VREF_GETIRQ('0'), AVR_VREF_IRQ_DAC0_MV),
			tiny3217_vref_to_dac_ac, mcu);

	/* TCD0 (12-bit timer type D) at 0x0A80: periodic OVF vector. */
	avr_tcd_init(avr, &mcu->tcd0, 0x0a80, TCD0_OVF_vect_num, '0');

	/* WDT (modern reset-only watchdog) at 0x0100. */
	avr_wdt_modern_init(avr, &mcu->wdt, 0x0100, '0');

	/* CRCSCAN (flash CRC memory scan) at 0x0120; 32 KB flash, APPEND/BOOTEND
	 * fuses (indices 7/8) bound the sections, CRC failure raises the NMI. */
	avr_crcscan_init(avr, &mcu->crcscan, 0x0120, 32768, 7, 8,
					 NMI_vect_num, '0');

	/* SLPCTRL (sleep controller) at 0x0050. */
	avr_slpctrl_init(avr, &mcu->slpctrl, 0x0050, '0');

	/* RSTCTRL (reset controller) at 0x0040. */
	avr_rstctrl_init(avr, &mcu->rstctrl, 0x0040, '0');

	/* BOD (brown-out detector / VLM) at 0x0080; CTRLA/B from FUSE.BODCFG
	 * (fuse index 1); BOD_VLM interrupt vector. */
	avr_bod_init(avr, &mcu->bod, 0x0080, BOD_VLM_vect_num, 1, '0');

	/* SYSCFG (REVID/EXTBRK) at 0x0F00 and the signature row (SIGROW) at 0x1100;
	 * DEVICEID is the device signature, revision A (0x00). */
	avr_syscfg_init(avr, &mcu->syscfg, 0x0f00, 0x1100, 0x00);
}

static void
tiny3217_reset(struct avr_t * avr)
{
	/* nothing extra yet */
	(void)avr;
}

const struct mcu_t SIM_CORENAME = {
	.core = {
		.mmcu = SIM_MMCU,
		MODERN_CORE(SIM_VECTOR_SIZE),

		.init = tiny3217_init,
		.reset = tiny3217_reset,
	},
};

static avr_t *
make(void)
{
	return avr_core_allocate(&SIM_CORENAME.core, sizeof(struct mcu_t));
}

avr_kind_t tiny3217 = {
	.names = { "attiny3217" },
	.make = make
};
