/*
	sim_avrdd.h

	Shared core template for the Microchip AVR(R) DD family (AVRxt, "AVR Dx"):
	AVR16/32DD14/20 (DS40002413B) and, with minor additions, AVR64DD14..32. The CPU,
	CPUINT and the timers/serial peripherals are the same AVRxt blocks as the
	tinyAVR 2-series; the clock controller, NVM controller, signature row and the
	multi-pin PORT configuration are AVR Dx specific (avr_clkctrl_dd,
	avr_nvmctrl_dd, avr_syscfg_dd, avr_port_modern_enable_multipin).

	A concrete core (cores/sim_avrNNddMM.c) is:

		#include "sim_avr.h"
		#define SIM_MMCU     "avrNNddMM"
		#define SIM_CORENAME mcu_avrNNddMM
		#define _AVR_IO_H_
		#define __ASSEMBLER__
		#define _SFR_MEM8(x)  (x)
		#define _SFR_MEM16(x) (x)
		#define _SFR_IO8(x)   (x)
		#define _SFR_IO16(x)  (x)
		#include "avr/ioavrNNddMM.h"
		#include "sim_avrdd.h"
		<kind declaration with .make = sim_avrdd_make>

	Not modelled yet (the registers read and write as plain memory): the TCD0 input modes/capture/blanking/PROGEV and CRC32 in CRCSCAN. See doc/avr_dd_support.md.

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

/* Vector entry size in bytes, taken from the device header (JMP = 4). */
#define SIM_VECTOR_SIZE	_VECTOR_SIZE

/* See sim_tinyx1.h: drop the identity helpers so sim_core_declare.h can define
 * its own. */
#undef _SFR_IO8
#undef _SFR_IO16
#undef _SFR_MEM8
#undef _BV
#undef _VECTOR

#include "sim_core_declare_modern.h"
#include "avr_clkctrl_dd.h"
#include "avr_nvmctrl_dd.h"
#include "avr_syscfg_dd.h"
#include "avr_tca.h"
#include "avr_tcb.h"
#include "avr_twi_modern.h"
#include "avr_usart_modern.h"
#include "avr_port_modern.h"
#include "avr_rtc.h"
#include "avr_spi_modern.h"
#include "avr_ccl.h"
#include "avr_wdt.h"
#include "avr_slpctrl.h"
#include "avr_cpuint.h"
#include "avr_rstctrl.h"
#include "avr_bod.h"
#include "avr_evsys.h"
#include "avr_portmux.h"
#include "avr_vref.h"
#include "avr_ac_dd.h"
#include "avr_dac.h"
#include "avr_zcd.h"
#include "avr_mvio.h"
#include "avr_tcd.h"
#include "avr_adc_dd.h"
#include "avr_crcscan.h"

/* Fuse indices (DS40002413B 8.8.1). */
#define SIM_FUSE_BODCFG		1
#define SIM_FUSE_OSCCFG		2
#define SIM_FUSE_SYSCFG0	5
#define SIM_FUSE_SYSCFG1	6
#define SIM_FUSE_CODESIZE	7
#define SIM_FUSE_BOOTSIZE	8

/* AVR DD fitted to the 14- and 20-pin packages: PORTA, C, D and F. */
struct mcu_t {
	avr_t				core;
	avr_clkctrl_dd_t	clkctrl;
	avr_nvmctrl_dd_t	nvmctrl;
	avr_port_modern_t	porta, portc, portd, portf;
	avr_port_modern_t	*ports[4];
	avr_tca_t			tca0;
	avr_tcb_t			tcb0, tcb1;
	avr_tcd_t			tcd0;
	avr_usart_modern_t	usart0, usart1;
	avr_twi_modern_t	twi;
	avr_spi_modern_t	spi0;
	avr_rtc_t			rtc;
	avr_ccl_t			ccl;
	avr_evsys_t			evsys;
	avr_portmux_t		portmux;
	avr_vref_t			vref;
	avr_adc_dd_t		adc0;
	avr_ac_dd_t			ac0;
	avr_dac_t			dac0;
	avr_zcd_t			zcd3;
	avr_mvio_t			mvio;
	avr_wdt_modern_t	wdt;
	avr_crcscan_t		crcscan;
	avr_slpctrl_t		slpctrl;
	avr_cpuint_t		cpuint;
	avr_rstctrl_t		rstctrl;
	avr_bod_t			bod;
	avr_syscfg_dd_t		syscfg;
};

/*
 * EVSYS generator values (CHANNELn, DS40002413B 16.5.2) and user indices
 * (USERxxx at 0x20 + n, 16.5.3).
 */
#define AVRDD_GEN_MVIO_OK		0x05
#define AVRDD_GEN_CCL_LUT0		0x10	/* LUT0..3 = 0x10..0x13 */
#define AVRDD_GEN_AC0_OUT		0x20
#define AVRDD_GEN_ADC0_RESRDY	0x24
#define AVRDD_GEN_ZCD3_OUT		0x30
#define AVRDD_GEN_TCB0_CAPT		0xa0
#define AVRDD_GEN_TCD0_CMPBCLR	0xb0	/* CMPBCLR, CMPASET, CMPBSET = 0xB0..0xB2 */
#define AVRDD_GEN_TCB1_CAPT		0xa2
#define AVRDD_USER_ADC0START	8
#define AVRDD_USER_TCA0CNTA		15
#define AVRDD_USER_TCB0CAPT		17
#define AVRDD_USER_TCB1CAPT		19
#define AVRDD_USER_TCD0INPUTA	23	/* INPUTA, INPUTB = 23, 24 */

static void
avrdd_gen_to_evsys(struct avr_irq_t * irq, uint32_t value, void * param, uint8_t gen)
{
	struct mcu_t * mcu = (struct mcu_t *)param;
	(void)irq;
	avr_evsys_async_generator(&mcu->evsys, gen, value & 1);
}

static void
avrdd_tcb0_capt_to_evsys(struct avr_irq_t * irq, uint32_t value, void * param)
{
	avrdd_gen_to_evsys(irq, value, param, AVRDD_GEN_TCB0_CAPT);
}

static void
avrdd_tcb1_capt_to_evsys(struct avr_irq_t * irq, uint32_t value, void * param)
{
	avrdd_gen_to_evsys(irq, value, param, AVRDD_GEN_TCB1_CAPT);
}

/* CCL LUTn output -> EVSYS generator 0x10 + n. */
static void
avrdd_ccl_lut_to_evsys(struct avr_irq_t * irq, uint32_t value, void * param)
{
	struct mcu_t * mcu = (struct mcu_t *)param;
	int lut = irq->irq - (mcu->ccl.base_irq + AVR_CCL_LUT_INPUTS * 4);
	if (lut >= 0 && lut < 4)
		avr_evsys_async_generator(&mcu->evsys, AVRDD_GEN_CCL_LUT0 + lut, value & 1);
}

static void
avrdd_ac0_to_evsys(struct avr_irq_t * irq, uint32_t value, void * param)
{
	avrdd_gen_to_evsys(irq, value, param, AVRDD_GEN_AC0_OUT);
}

static void
avrdd_zcd3_to_evsys(struct avr_irq_t * irq, uint32_t value, void * param)
{
	avrdd_gen_to_evsys(irq, value, param, AVRDD_GEN_ZCD3_OUT);
}

static void
avrdd_mvio_to_evsys(struct avr_irq_t * irq, uint32_t value, void * param)
{
	avrdd_gen_to_evsys(irq, value, param, AVRDD_GEN_MVIO_OK);
}

/* VREF.ACREF -> the AC0 DACREF divider; VREF.DAC0REF -> DAC0. */
static void
avrdd_vref_to_ac(struct avr_irq_t * irq, uint32_t value, void * param)
{
	struct mcu_t * mcu = (struct mcu_t *)param;
	(void)irq;
	avr_ac_dd_set_vref(&mcu->ac0, value);
}

static void
avrdd_vref_to_dac(struct avr_irq_t * irq, uint32_t value, void * param)
{
	struct mcu_t * mcu = (struct mcu_t *)param;
	(void)irq;
	avr_dac_set_vref(&mcu->dac0, value);
}

static void
avrdd_adc_res_to_evsys(struct avr_irq_t * irq, uint32_t value, void * param)
{
	avrdd_gen_to_evsys(irq, value, param, AVRDD_GEN_ADC0_RESRDY);
}

/* EVSYS ADC0START user: a rising event edge starts a conversion (STARTEI). */
static void
avrdd_evsys_to_adc(struct avr_irq_t * irq, uint32_t value, void * param)
{
	struct mcu_t * mcu = (struct mcu_t *)param;
	(void)irq;
	if (value & 1)
		avr_adc_dd_event_start(&mcu->adc0);
}

/* VREF.ADC0REF -> the ADC reference. */
static void
avrdd_vref_to_adc(struct avr_irq_t * irq, uint32_t value, void * param)
{
	struct mcu_t * mcu = (struct mcu_t *)param;
	(void)irq;
	avr_adc_dd_set_ref(&mcu->adc0, value);
}

static void
avrdd_evsys_to_tcb0(struct avr_irq_t * irq, uint32_t value, void * param)
{
	struct mcu_t * mcu = (struct mcu_t *)param;
	(void)irq;
	avr_raise_irq(avr_io_getirq(&mcu->core, AVR_IOCTL_TCB_GETIRQ('0'),
								AVR_TCB_IRQ_EVENT_IN), value & 1);
}

static void
avrdd_evsys_to_tcb1(struct avr_irq_t * irq, uint32_t value, void * param)
{
	struct mcu_t * mcu = (struct mcu_t *)param;
	(void)irq;
	avr_raise_irq(avr_io_getirq(&mcu->core, AVR_IOCTL_TCB_GETIRQ('1'),
								AVR_TCB_IRQ_EVENT_IN), value & 1);
}

/* TCD0 CLKSEL source frequency (DS40002413B 25.5.1): OSCHF, PLL or EXTCLK. */
static uint32_t
avrdd_tcd_clock(void * param, uint8_t clksel)
{
	struct mcu_t * mcu = (struct mcu_t *)param;

	switch (clksel) {
	case 0:	return avr_clkctrl_dd_oschf_hz(&mcu->clkctrl);
	case 1:	return avr_clkctrl_dd_pll_hz(&mcu->clkctrl);
	default:	return mcu->clkctrl.freq_extclk;
	}
}

/* TCD0 CMPBCLR / CMPASET / CMPBSET match pulses -> EVSYS generators 0xB0..0xB2. */
static void
avrdd_tcd0_event_to_evsys(struct avr_irq_t * irq, uint32_t value, void * param)
{
	struct mcu_t * mcu = (struct mcu_t *)param;
	int ev = irq->irq - mcu->tcd0.base_irq - AVR_TCD_IRQ_EVT_CMPBCLR;
	avr_evsys_async_generator(&mcu->evsys, AVRDD_GEN_TCD0_CMPBCLR + ev, value & 1);
}

/* A BOD brown-out resets the device through RSTCTRL, recorded in RSTFR.BORF. */
static void
avrdd_bod_brownout(struct avr_t * avr, void * param)
{
	struct mcu_t * mcu = (struct mcu_t *)param;
	(void)avr;
	avr_rstctrl_request_reset(&mcu->rstctrl, AVR_RSTCTRL_BORF);
}

static void
avrdd_wdt_reset(struct avr_t * avr, void * param)
{
	struct mcu_t * mcu = (struct mcu_t *)param;
	(void)avr;
	avr_rstctrl_request_reset(&mcu->rstctrl, AVR_RSTCTRL_WDRF);
}

static void
avrdd_init(struct avr_t * avr)
{
	struct mcu_t * mcu = (struct mcu_t *)avr;

	/* SYSCFG/SIGROW/FUSE/LOCK first: it loads the factory fuse defaults that
	 * CLKCTRL, NVMCTRL and CPUINT consult. Revision A0 (placeholder). */
	avr_syscfg_dd_init(avr, &mcu->syscfg, 0x0f00, 0x1100, 0x1050, 0x1040, 0x10);

	/* CLKCTRL at 0x60; start-up clock from FUSE.OSCCFG. */
	avr_clkctrl_dd_init(avr, &mcu->clkctrl, 0x0060, SIM_FUSE_OSCCFG);

	/* NVMCTRL at 0x1000: EEPROM at 0x1400, USERROW at 0x1080 and the flash
	 * mapped into data space at 0x8000 (512-byte pages). */
	avr_nvmctrl_dd_init(avr, &mcu->nvmctrl, 0x1000,
						EEPROM_START, EEPROM_SIZE,
						USER_SIGNATURES_START, USER_SIGNATURES_SIZE,
						MAPPED_PROGMEM_START, FLASHEND + 1, PROGMEM_PAGE_SIZE,
						SIM_FUSE_CODESIZE, SIM_FUSE_BOOTSIZE, NVMCTRL_EE_vect_num);

	/* PORTA/C/D/F at 0x400/0x440/0x460/0x4A0 with VPORTA/C/D/F at 0x00/0x08/
	 * 0x0C/0x14 (DS40002413B Table 9-1). */
	avr_port_modern_init(avr, &mcu->porta, 'A', 0x0400, 0x0000, PORTA_PORT_vect_num);
	avr_port_modern_init(avr, &mcu->portc, 'C', 0x0440, 0x0008, PORTC_PORT_vect_num);
	avr_port_modern_init(avr, &mcu->portd, 'D', 0x0460, 0x000c, PORTD_PORT_vect_num);
	avr_port_modern_init(avr, &mcu->portf, 'F', 0x04a0, 0x0014, PORTF_PORT_vect_num);
	mcu->ports[0] = &mcu->porta;
	mcu->ports[1] = &mcu->portc;
	mcu->ports[2] = &mcu->portd;
	mcu->ports[3] = &mcu->portf;
	avr_port_modern_enable_multipin(avr, mcu->ports, 4);

	/* TCA0 at 0xA00 (single mode): OVF + CMP0/1/2. */
	avr_tca_init(avr, &mcu->tca0, 0x0a00, TCA0_OVF_vect_num,
				 TCA0_CMP0_vect_num, TCA0_CMP1_vect_num, TCA0_CMP2_vect_num, '0');

	/* TCB0/TCB1 at 0xB00/0xB10. */
	avr_tcb_init(avr, &mcu->tcb0, 0x0b00, TCB0_INT_vect_num, '0');
	avr_tcb_init(avr, &mcu->tcb1, 0x0b10, TCB1_INT_vect_num, '1');

	/* TCD0 at 0xB80: OVF interrupt. The clock source follows CLKCTRL. */
	avr_tcd_init(avr, &mcu->tcd0, 0x0b80, TCD0_OVF_vect_num, 0xff, '0');
	mcu->tcd0.clk_fn = avrdd_tcd_clock;
	mcu->tcd0.clk_param = mcu;

	/* USART0/1 at 0x800/0x820. */
	avr_usart_modern_init(avr, &mcu->usart0, 0x0800,
						  USART0_RXC_vect_num, USART0_DRE_vect_num, USART0_TXC_vect_num, '0');
	avr_usart_modern_init(avr, &mcu->usart1, 0x0820,
						  USART1_RXC_vect_num, USART1_DRE_vect_num, USART1_TXC_vect_num, '1');

	/* TWI0 at 0x900, SPI0 at 0x940. */
	avr_twi_modern_init(avr, &mcu->twi, 0x0900,
						TWI0_TWIM_vect_num, TWI0_TWIS_vect_num, '0');
	avr_twi_modern_enable_dual(avr, &mcu->twi);
	avr_spi_modern_init(avr, &mcu->spi0, 0x0940, SPI0_INT_vect_num, '0');

	/* RTC + PIT at 0x0140. */
	avr_rtc_init(avr, &mcu->rtc, 0x0140, RTC_CNT_vect_num, RTC_PIT_vect_num, '0');

	/* CCL at 0x01C0: 4 LUTs, one CCL_CCL interrupt. */
	avr_ccl_init_dd(avr, &mcu->ccl, 0x01c0, 4, '0');
	avr_ccl_set_interrupt(avr, &mcu->ccl, CCL_CCL_vect_num);

	/* EVSYS at 0x0200: six channels, 25 users (DS40002413B 16.5). */
	avr_evsys_init_xt(avr, &mcu->evsys, 0x0200, '0', 6, 25);
	avr_irq_register_notify(
			avr_io_getirq(avr, AVR_IOCTL_TCB_GETIRQ('0'), AVR_TCB_IRQ_CAPT_OUT),
			avrdd_tcb0_capt_to_evsys, mcu);
	avr_irq_register_notify(
			avr_io_getirq(avr, AVR_IOCTL_TCB_GETIRQ('1'), AVR_TCB_IRQ_CAPT_OUT),
			avrdd_tcb1_capt_to_evsys, mcu);
	for (int lut = 0; lut < 4; lut++)
		avr_irq_register_notify(
				avr_io_getirq(avr, AVR_IOCTL_CCL_GETIRQ('0'),
							  AVR_CCL_LUT_INPUTS * 4 + lut),
				avrdd_ccl_lut_to_evsys, mcu);
	avr_irq_register_notify(
			avr_io_getirq(avr, AVR_IOCTL_EVSYS_GETIRQ('0'),
						  AVR_EVSYS_IRQ_USER0 + AVRDD_USER_TCB0CAPT),
			avrdd_evsys_to_tcb0, mcu);
	avr_irq_register_notify(
			avr_io_getirq(avr, AVR_IOCTL_EVSYS_GETIRQ('0'),
						  AVR_EVSYS_IRQ_USER0 + AVRDD_USER_TCB1CAPT),
			avrdd_evsys_to_tcb1, mcu);
	avr_connect_irq(
			avr_io_getirq(avr, AVR_IOCTL_EVSYS_GETIRQ('0'),
						  AVR_EVSYS_IRQ_USER0 + AVRDD_USER_TCA0CNTA),
			avr_io_getirq(avr, AVR_IOCTL_TCA_GETIRQ('0'), AVR_TCA_IRQ_EV_IN));

	/* PORTMUX at 0x05E0 (offsets 0-9). Routed outputs: USART0/1 TxD
	 * (USARTROUTEA, 17.3.3) and TCA0 WO0-5 (TCAROUTEA, 17.3.6). Unavailable
	 * pin choices are left empty and route nowhere. */
	avr_portmux_init_n(avr, &mcu->portmux, 0x05e0, 10, '0');
	avr_portmux_set_port(&mcu->portmux, 'A', &mcu->porta);
	avr_portmux_set_port(&mcu->portmux, 'C', &mcu->portc);
	avr_portmux_set_port(&mcu->portmux, 'D', &mcu->portd);
	avr_portmux_set_port(&mcu->portmux, 'F', &mcu->portf);
	{
		static const avr_portmux_pin_t usart0_txd[8] = {
			{ 'A', 0 }, { 'A', 4 }, { 'A', 2 }, { 'D', 4 }, { 'C', 1 },
		};
		static const avr_portmux_pin_t usart1_txd[4] = {
			{ 'C', 1 }, { 0, 0 }, { 'D', 6 },
		};
		int fn = avr_portmux_add_func(&mcu->portmux, 2 /* USARTROUTEA */, 0x07, 0,
									  usart0_txd, 8, 0);
		avr_portmux_bind_irq(&mcu->portmux, fn,
			avr_io_getirq(avr, AVR_IOCTL_UART_GETIRQ('0'), USART_MODERN_IRQ_TXD_PIN));
		fn = avr_portmux_add_func(&mcu->portmux, 2, 0x18, 3, usart1_txd, 4, 0);
		avr_portmux_bind_irq(&mcu->portmux, fn,
			avr_io_getirq(avr, AVR_IOCTL_UART_GETIRQ('1'), USART_MODERN_IRQ_TXD_PIN));
	}
	{
		/* TCAROUTEA.TCA0: 0 = PORTA (PA0-5), 2 = PORTC (PC1-3), 3 = PORTD (PD4-5). */
		static const avr_portmux_pin_t wo[6][4] = {
			{ { 'A', 0 } },
			{ { 'A', 1 }, { 0, 0 }, { 'C', 1 } },
			{ { 'A', 2 }, { 0, 0 }, { 'C', 2 } },
			{ { 'A', 3 }, { 0, 0 }, { 'C', 3 } },
			{ { 'A', 4 }, { 0, 0 }, { 0, 0 }, { 'D', 4 } },
			{ { 'A', 5 }, { 0, 0 }, { 0, 0 }, { 'D', 5 } },
		};
		for (int ch = 0; ch < 6; ch++) {
			int fn = avr_portmux_add_func(&mcu->portmux, 7 /* TCAROUTEA */, 0x07, 0,
										  wo[ch], 4, 0 /* DIR is the user's */);
			avr_portmux_bind_irq(&mcu->portmux, fn,
				avr_io_getirq(avr, AVR_IOCTL_TCA_GETIRQ('0'), AVR_TCA_IRQ_PIN0 + ch));
		}
	}

	/* VREF at 0x00B0: ADC0REF / DAC0REF / ACREF feed their consumers. */
	avr_vref_init_dd(avr, &mcu->vref, 0x00b0, '0');
	avr_irq_register_notify(
			avr_io_getirq(avr, AVR_IOCTL_VREF_GETIRQ('0'), AVR_VREF_IRQ_DD_AC0_MV),
			avrdd_vref_to_ac, mcu);
	avr_irq_register_notify(
			avr_io_getirq(avr, AVR_IOCTL_VREF_GETIRQ('0'), AVR_VREF_IRQ_DAC0_MV),
			avrdd_vref_to_dac, mcu);

	/* AC0 at 0x0680, DAC0 at 0x06A0, ZCD3 at 0x06D8, MVIO at 0x00C0. */
	avr_ac_dd_init(avr, &mcu->ac0, 0x0680, AC0_AC_vect_num, '0');
	avr_dac_init_dd(avr, &mcu->dac0, 0x06a0, '0');
	avr_zcd_init(avr, &mcu->zcd3, 0x06d8, ZCD3_ZCD_vect_num, '3');
	avr_mvio_init(avr, &mcu->mvio, 0x00c0, MVIO_MVIO_vect_num, SIM_FUSE_SYSCFG1, '0');
	avr_irq_register_notify(
			avr_io_getirq(avr, AVR_IOCTL_AC_GETIRQ('0'), AVR_AC_DD_IRQ_OUT),
			avrdd_ac0_to_evsys, mcu);
	avr_irq_register_notify(
			avr_io_getirq(avr, AVR_IOCTL_ZCD_GETIRQ('3'), AVR_ZCD_IRQ_OUT),
			avrdd_zcd3_to_evsys, mcu);
	avr_irq_register_notify(
			avr_io_getirq(avr, AVR_IOCTL_MVIO_GETIRQ('0'), AVR_MVIO_IRQ_OK),
			avrdd_mvio_to_evsys, mcu);

	/* ADC0 at 0x0600: RESRDY and WCMP vectors; its DAC0 and DACREF0 inputs follow
	 * DAC0 and the AC0 DACREF divider; ADC0REF selects the reference. */
	avr_adc_dd_init(avr, &mcu->adc0, 0x0600, ADC0_RESRDY_vect_num,
					ADC0_WCMP_vect_num, '0');
	avr_adc_dd_set_tempsense(&mcu->adc0, SIGROW_TEMPSENSE0);
	avr_adc_dd_set_dacref_source(&mcu->adc0, &mcu->ac0.dacref_mv);
	avr_connect_irq(
			avr_io_getirq(avr, AVR_IOCTL_DAC_GETIRQ('0'), AVR_DAC_IRQ_OUT),
			avr_io_getirq(avr, AVR_IOCTL_ADC_DD_GETIRQ('0'), AVR_ADC_DD_CH_DAC0));
	avr_irq_register_notify(
			avr_io_getirq(avr, AVR_IOCTL_VREF_GETIRQ('0'), AVR_VREF_IRQ_ADC0_MV),
			avrdd_vref_to_adc, mcu);
	avr_irq_register_notify(
			avr_io_getirq(avr, AVR_IOCTL_ADC_DD_GETIRQ('0'), AVR_ADC_DD_IRQ_EVT_RES),
			avrdd_adc_res_to_evsys, mcu);
	avr_irq_register_notify(
			avr_io_getirq(avr, AVR_IOCTL_EVSYS_GETIRQ('0'),
						  AVR_EVSYS_IRQ_USER0 + AVRDD_USER_ADC0START),
			avrdd_evsys_to_adc, mcu);

	/* WDT at 0x0100 (reset-only). */
	avr_wdt_modern_init(avr, &mcu->wdt, 0x0100, '0');
	avr_wdt_modern_set_reset_handler(&mcu->wdt, avrdd_wdt_reset, mcu);

	/* Live peripheral outputs into the CCL input-source MUX (DS40002413B 31.5.7):
	 * AC0 OUT = INSEL 0x6, ZCD3 OUT = 0x7, TCA0 WO0-2 = 0xA, TCB0/1 WO = 0xB. The
	 * USART TxD, SPI and TCD0 waveform outputs have no line-level IRQs yet. */
	avr_connect_irq(
			avr_io_getirq(avr, AVR_IOCTL_AC_GETIRQ('0'), AVR_AC_DD_IRQ_OUT),
			avr_io_getirq(avr, AVR_IOCTL_CCL_GETIRQ('0'),
						  AVR_CCL_IRQ_SRC_4LUT(AVR_CCL_SRC_AC0)));
	avr_connect_irq(
			avr_io_getirq(avr, AVR_IOCTL_ZCD_GETIRQ('3'), AVR_ZCD_IRQ_OUT),
			avr_io_getirq(avr, AVR_IOCTL_CCL_GETIRQ('0'),
						  AVR_CCL_IRQ_SRC_4LUT(AVR_CCL_SRC_ZCD3)));
	for (int wo = 0; wo < 3; wo++)
		avr_connect_irq(
				avr_io_getirq(avr, AVR_IOCTL_TCA_GETIRQ('0'), AVR_TCA_IRQ_WO0 + wo),
				avr_io_getirq(avr, AVR_IOCTL_CCL_GETIRQ('0'),
							  AVR_CCL_IRQ_SRC_4LUT(AVR_CCL_SRC_TCA0_WO0 + wo)));
	for (int tcb = 0; tcb < 2; tcb++)
		avr_connect_irq(
				avr_io_getirq(avr, AVR_IOCTL_TCB_GETIRQ('0' + tcb), AVR_TCB_IRQ_WO),
				avr_io_getirq(avr, AVR_IOCTL_CCL_GETIRQ('0'),
							  AVR_CCL_IRQ_SRC_4LUT(AVR_CCL_SRC_TCB0 + tcb)));

	/* TCD0 events and interrupts: the TRIG vector, the CMPBCLR/CMPASET/CMPBSET
	 * generators and the INPUTA / INPUTB users (DS40002413B 25.3.4, 16.5). */
	avr_tcd_set_trig_vector(avr, &mcu->tcd0, TCD0_TRIG_vect_num);
	for (int ev = AVR_TCD_IRQ_EVT_CMPBCLR; ev <= AVR_TCD_IRQ_EVT_CMPBSET; ev++)
		avr_irq_register_notify(
				avr_io_getirq(avr, AVR_IOCTL_TCD_GETIRQ('0'), ev),
				avrdd_tcd0_event_to_evsys, mcu);
	avr_connect_irq(
			avr_io_getirq(avr, AVR_IOCTL_EVSYS_GETIRQ('0'),
						  AVR_EVSYS_IRQ_USER0 + AVRDD_USER_TCD0INPUTA),
			avr_io_getirq(avr, AVR_IOCTL_TCD_GETIRQ('0'), AVR_TCD_IRQ_IN_A));
	avr_connect_irq(
			avr_io_getirq(avr, AVR_IOCTL_EVSYS_GETIRQ('0'),
						  AVR_EVSYS_IRQ_USER0 + AVRDD_USER_TCD0INPUTA + 1),
			avr_io_getirq(avr, AVR_IOCTL_TCD_GETIRQ('0'), AVR_TCD_IRQ_IN_B));
	{
		/* TCD0 WOA / WOB / WOC -> CCL INSEL 0xC of inputs 0 / 1 / 2. The CCL source
		 * enum keeps WOC apart from WOA / WOB, so list them explicitly. */
		static const uint8_t srcs[3] = {
			AVR_CCL_SRC_TCD0_WOA, AVR_CCL_SRC_TCD0_WOB, AVR_CCL_SRC_TCD0_WOC,
		};
		for (int i = 0; i < 3; i++)
			avr_connect_irq(
					avr_io_getirq(avr, AVR_IOCTL_TCD_GETIRQ('0'), AVR_TCD_IRQ_WOA + i),
					avr_io_getirq(avr, AVR_IOCTL_CCL_GETIRQ('0'),
								  AVR_CCL_IRQ_SRC_4LUT(srcs[i])));
	}

	/* CRCSCAN at 0x0120: BOOTSIZE / CODESIZE (512-byte units) bound the sections;
	 * a CRC failure raises the NMI. */
	avr_crcscan_init_dd(avr, &mcu->crcscan, 0x0120, FLASHEND + 1, SIM_FUSE_SYSCFG0,
						SIM_FUSE_CODESIZE, SIM_FUSE_BOOTSIZE, NMI_vect_num, '0');

	/* SLPCTRL at 0x0050, RSTCTRL at 0x0040. */
	avr_slpctrl_init(avr, &mcu->slpctrl, 0x0050, '0');
	avr_rstctrl_init(avr, &mcu->rstctrl, 0x0040, '0');

	/* CPUINT at 0x0110. FUSE.BOOTSIZE (index 8) in 512-byte blocks places the
	 * application vector table (DS40002413B 11.3.1.1 note 1). */
	avr_cpuint_init(avr, &mcu->cpuint, 0x0110, SIM_FUSE_BOOTSIZE, '0');
	avr_cpuint_set_bootend_unit(avr, 512);

	/* BOD / VLM at 0x00A0; CTRLA/B from FUSE.BODCFG. */
	avr_bod_init(avr, &mcu->bod, 0x00a0, BOD_VLM_vect_num, SIM_FUSE_BODCFG, '0');
	avr_bod_set_brownout_handler(&mcu->bod, avrdd_bod_brownout, mcu);
}

static void
avrdd_reset(struct avr_t * avr)
{
	(void)avr;
}

const struct mcu_t SIM_CORENAME = {
	.core = {
		.mmcu = SIM_MMCU,
		MODERN_CORE(SIM_VECTOR_SIZE),

		.init = avrdd_init,
		.reset = avrdd_reset,
	},
};

/* The kind descriptor is declared in each cores/sim_avrNNddMM.c using this helper. */
static avr_t *
sim_avrdd_make(void)
{
	return avr_core_allocate(&SIM_CORENAME.core, sizeof(struct mcu_t));
}
