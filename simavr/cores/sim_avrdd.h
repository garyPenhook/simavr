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

	Not modelled yet (the registers read and write as plain memory): EVSYS,
	PORTMUX, VREF, ADC0, AC0, DAC0, ZCD3, MVIO, TCD0, CRCSCAN, and the TWI dual
	mode. See doc/avr_dd_support.md.

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

/* Fuse indices (DS40002413B 8.8.1). */
#define SIM_FUSE_BODCFG		1
#define SIM_FUSE_OSCCFG		2
#define SIM_FUSE_SYSCFG0	5
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
	avr_usart_modern_t	usart0, usart1;
	avr_twi_modern_t	twi;
	avr_spi_modern_t	spi0;
	avr_rtc_t			rtc;
	avr_ccl_t			ccl;
	avr_wdt_modern_t	wdt;
	avr_slpctrl_t		slpctrl;
	avr_cpuint_t		cpuint;
	avr_rstctrl_t		rstctrl;
	avr_bod_t			bod;
	avr_syscfg_dd_t		syscfg;
};

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

	/* USART0/1 at 0x800/0x820. */
	avr_usart_modern_init(avr, &mcu->usart0, 0x0800,
						  USART0_RXC_vect_num, USART0_DRE_vect_num, USART0_TXC_vect_num, '0');
	avr_usart_modern_init(avr, &mcu->usart1, 0x0820,
						  USART1_RXC_vect_num, USART1_DRE_vect_num, USART1_TXC_vect_num, '1');

	/* TWI0 at 0x900, SPI0 at 0x940. */
	avr_twi_modern_init(avr, &mcu->twi, 0x0900,
						TWI0_TWIM_vect_num, TWI0_TWIS_vect_num, '0');
	avr_spi_modern_init(avr, &mcu->spi0, 0x0940, SPI0_INT_vect_num, '0');

	/* RTC + PIT at 0x0140. */
	avr_rtc_init(avr, &mcu->rtc, 0x0140, RTC_CNT_vect_num, RTC_PIT_vect_num, '0');

	/* CCL at 0x01C0: 4 LUTs, one CCL_CCL interrupt. */
	avr_ccl_init_mega(avr, &mcu->ccl, 0x01c0, 4, '0');
	avr_ccl_set_interrupt(avr, &mcu->ccl, CCL_CCL_vect_num);

	/* WDT at 0x0100 (reset-only). */
	avr_wdt_modern_init(avr, &mcu->wdt, 0x0100, '0');
	avr_wdt_modern_set_reset_handler(&mcu->wdt, avrdd_wdt_reset, mcu);

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
