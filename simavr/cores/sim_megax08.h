/*
	sim_megax08.h

	Shared core template for the Microchip megaAVR(R) 0-series (AVRxt): ATmega808,
	809, 1608, 1609, 3208, 3209, 4808, 4809. Same modern (AVRxt) CPU and modern
	peripheral architecture as the tinyAVR 0/1-series, but a different memory map
	and peripheral mix: six ports (A-F), up to four USARTs and four TCBs, and NO
	TCD/DAC. As with the tinyAVR template, every device-specific value is taken
	from the part's own avr-libc header (generated from the same Microchip device
	files as the datasheets): memory sizes, signature, the interrupt vector table,
	and which instances are fitted (USART3 / TCB3 on the 48-pin "x09" parts).

	A concrete core (cores/sim_megaNNN.c) is just:

		#include "sim_avr.h"
		#define SIM_MMCU     "atmegaNNN"
		#define SIM_CORENAME mcu_megaNNN
		#define _AVR_IO_H_
		#define __ASSEMBLER__
		#define _SFR_MEM8(x)  (x)
		#define _SFR_MEM16(x) (x)
		#define _SFR_IO8(x)   (x)
		#define _SFR_IO16(x)  (x)
		#include "avr/iomNNN.h"
		#include "sim_megax08.h"
		<literal kind declaration the build's core-table generator can grep for>

	Not present on this family: TCD and DAC.

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
 * The megaAVR 0-series toolchain emits 4-byte JMP-based interrupt vectors on
 * EVERY part, including the 8 KB ATmega808/809 (unlike the tinyAVR parts, where
 * <=8 KB devices use 2-byte RJMP vectors). Verified by avr-objdump of the linked
 * vector table: spacing is 4 bytes for 808/809/1608/.../4809. Using 2 here would
 * send the CPUINT dispatch (pc = vector * vector_size) to the wrong entries and
 * interrupts would never reach their handlers.
 */
#define SIM_VECTOR_SIZE	4

#define SIM_EE_SIZE	(E2END - EEPROM_START + 1)

/* See note in sim_tinyx1.h: the identity _SFR_xxx, _BV and _VECTOR helpers from
 * the including .c are dropped here so sim_core_declare.h can define its own. */
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
 * The device structure. All megaAVR-0 parts carry six ports (A-F), TCA0,
 * TCB0/1/2, USART0/1/2, TWI0, SPI0, RTC, ADC0, AC0 and the system peripherals;
 * the 48-pin "x09" parts additionally fit USART3 and TCB3.
 */
struct mcu_t {
	avr_t				core;
	avr_clkctrl_t		clkctrl;
	avr_nvmctrl_t		nvmctrl;
	avr_port_modern_t	porta, portb, portc, portd, porte, portf;
	avr_tca_t			tca0;
	avr_tcb_t			tcb0, tcb1, tcb2;
#ifdef TCB3_INT_vect_num
	avr_tcb_t			tcb3;
#endif
	avr_usart_modern_t	usart0, usart1, usart2;
#ifdef USART3_RXC_vect_num
	avr_usart_modern_t	usart3;
#endif
	avr_twi_modern_t	twi;
	avr_rtc_t			rtc;
	avr_adc_modern_t	adc0;
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

/* VREF.ADC0REFSEL -> ADC0 internal reference (used when CTRLC.REFSEL = INTREF). */
static void
megax08_vref_to_adc(struct avr_irq_t * irq, uint32_t value, void * param)
{
	struct mcu_t * mcu = (struct mcu_t *)param;
	(void)irq;
	avr_adc_modern_set_intref(&mcu->adc0, value);
}

static void
megax08_ac0_to_evsys(struct avr_irq_t * irq, uint32_t value, void * param)
{
	struct mcu_t * mcu = (struct mcu_t *)param;
	(void)irq;
	avr_evsys_async_generator(&mcu->evsys, 0x20 /* AC0 OUT */, value & 1);
}

static void
megax08_evsys_to_adc(struct avr_irq_t * irq, uint32_t value, void * param)
{
	struct mcu_t * mcu = (struct mcu_t *)param;
	(void)irq;
	if (value & 1)
		avr_adc_modern_event_start(&mcu->adc0);
}

static void
megax08_tcb0_capt_to_evsys(struct avr_irq_t * irq, uint32_t value, void * param)
{
	struct mcu_t * mcu = (struct mcu_t *)param;
	(void)irq;
	avr_evsys_async_generator(&mcu->evsys, 0xa0, value & 1);
}

static void
megax08_tcb1_capt_to_evsys(struct avr_irq_t * irq, uint32_t value, void * param)
{
	struct mcu_t * mcu = (struct mcu_t *)param;
	(void)irq;
	avr_evsys_async_generator(&mcu->evsys, 0xa2, value & 1);
}

static void
megax08_tcb2_capt_to_evsys(struct avr_irq_t * irq, uint32_t value, void * param)
{
	struct mcu_t * mcu = (struct mcu_t *)param;
	(void)irq;
	avr_evsys_async_generator(&mcu->evsys, 0xa4, value & 1);
}

#ifdef TCB3_INT_vect_num
static void
megax08_tcb3_capt_to_evsys(struct avr_irq_t * irq, uint32_t value, void * param)
{
	struct mcu_t * mcu = (struct mcu_t *)param;
	(void)irq;
	avr_evsys_async_generator(&mcu->evsys, 0xa6, value & 1);
}
#endif

static void
megax08_evsys_to_tcb0(struct avr_irq_t * irq, uint32_t value, void * param)
{
	struct mcu_t * mcu = (struct mcu_t *)param;
	(void)irq;
	avr_raise_irq(avr_io_getirq(&mcu->core, AVR_IOCTL_TCB_GETIRQ('0'),
								AVR_TCB_IRQ_EVENT_IN), value & 1);
}

static void
megax08_evsys_to_tcb1(struct avr_irq_t * irq, uint32_t value, void * param)
{
	struct mcu_t * mcu = (struct mcu_t *)param;
	(void)irq;
	avr_raise_irq(avr_io_getirq(&mcu->core, AVR_IOCTL_TCB_GETIRQ('1'),
								AVR_TCB_IRQ_EVENT_IN), value & 1);
}

static void
megax08_evsys_to_tcb2(struct avr_irq_t * irq, uint32_t value, void * param)
{
	struct mcu_t * mcu = (struct mcu_t *)param;
	(void)irq;
	avr_raise_irq(avr_io_getirq(&mcu->core, AVR_IOCTL_TCB_GETIRQ('2'),
								AVR_TCB_IRQ_EVENT_IN), value & 1);
}

#ifdef TCB3_INT_vect_num
static void
megax08_evsys_to_tcb3(struct avr_irq_t * irq, uint32_t value, void * param)
{
	struct mcu_t * mcu = (struct mcu_t *)param;
	(void)irq;
	avr_raise_irq(avr_io_getirq(&mcu->core, AVR_IOCTL_TCB_GETIRQ('3'),
								AVR_TCB_IRQ_EVENT_IN), value & 1);
}
#endif

/*
 * A BOD brown-out resets the device through RSTCTRL, recorded in RSTFR.BORF.
 */
static void
megax08_bod_brownout(struct avr_t * avr, void * param)
{
	struct mcu_t * mcu = (struct mcu_t *)param;
	(void)avr;
	avr_rstctrl_request_reset(&mcu->rstctrl, AVR_RSTCTRL_BORF);
}

static void
megax08_wdt_reset(struct avr_t * avr, void * param)
{
	struct mcu_t * mcu = (struct mcu_t *)param;
	(void)avr;
	avr_rstctrl_request_reset(&mcu->rstctrl, AVR_RSTCTRL_WDRF);
}

static void
megax08_init(struct avr_t * avr)
{
	struct mcu_t * mcu = (struct mcu_t *)avr;

	/* CLKCTRL at 0x60; OSC20M base from FUSE.OSCCFG (fuse index 2). */
	avr_clkctrl_init(avr, &mcu->clkctrl, 0x0060, 2);

	/* NVMCTRL at 0x1000; EEPROM mapped at EEPROM_START (256 bytes). */
	avr_nvmctrl_init(avr, &mcu->nvmctrl, 0x1000, EEPROM_START, SIM_EE_SIZE,
					 NVMCTRL_EE_vect_num);
	/* Flash self-programming; megaAVR-0 maps flash into data space at 0x4000. */
	avr_nvmctrl_set_flash(&mcu->nvmctrl, MAPPED_PROGMEM_START,
						  FLASHEND + 1, PROGMEM_PAGE_SIZE);

	/* PORTA..F at 0x400..0x4A0 (+ VPORTA..F at 0x00..0x14). */
	avr_port_modern_init(avr, &mcu->porta, 'A', 0x0400, 0x0000, PORTA_PORT_vect_num);
	avr_port_modern_init(avr, &mcu->portb, 'B', 0x0420, 0x0004, PORTB_PORT_vect_num);
	avr_port_modern_init(avr, &mcu->portc, 'C', 0x0440, 0x0008, PORTC_PORT_vect_num);
	avr_port_modern_init(avr, &mcu->portd, 'D', 0x0460, 0x000c, PORTD_PORT_vect_num);
	avr_port_modern_init(avr, &mcu->porte, 'E', 0x0480, 0x0010, PORTE_PORT_vect_num);
	avr_port_modern_init(avr, &mcu->portf, 'F', 0x04a0, 0x0014, PORTF_PORT_vect_num);

	/* TCA0 at 0xA00 (single mode): OVF + CMP0/1/2. */
	avr_tca_init(avr, &mcu->tca0, 0x0a00, TCA0_OVF_vect_num,
				 TCA0_CMP0_vect_num, TCA0_CMP1_vect_num, TCA0_CMP2_vect_num, '0');

	/* TCB0/1/2 at 0xA80/0xA90/0xAA0; TCB3 at 0xAB0 on the x09 parts. */
	avr_tcb_init(avr, &mcu->tcb0, 0x0a80, TCB0_INT_vect_num, '0');
	avr_tcb_init(avr, &mcu->tcb1, 0x0a90, TCB1_INT_vect_num, '1');
	avr_tcb_init(avr, &mcu->tcb2, 0x0aa0, TCB2_INT_vect_num, '2');
#ifdef TCB3_INT_vect_num
	avr_tcb_init(avr, &mcu->tcb3, 0x0ab0, TCB3_INT_vect_num, '3');
#endif

	/* USART0/1/2 at 0x800/0x820/0x840; USART3 at 0x860 on the x09 parts. */
	avr_usart_modern_init(avr, &mcu->usart0, 0x0800,
						  USART0_RXC_vect_num, USART0_DRE_vect_num, USART0_TXC_vect_num, '0');
	avr_usart_modern_init(avr, &mcu->usart1, 0x0820,
						  USART1_RXC_vect_num, USART1_DRE_vect_num, USART1_TXC_vect_num, '1');
	avr_usart_modern_init(avr, &mcu->usart2, 0x0840,
						  USART2_RXC_vect_num, USART2_DRE_vect_num, USART2_TXC_vect_num, '2');
#ifdef USART3_RXC_vect_num
	avr_usart_modern_init(avr, &mcu->usart3, 0x0860,
						  USART3_RXC_vect_num, USART3_DRE_vect_num, USART3_TXC_vect_num, '3');
#endif

	/* TWI0 at 0x8A0; SPI0 at 0x8C0 (note: not the tinyAVR addresses). */
	avr_twi_modern_init(avr, &mcu->twi, 0x08a0,
						TWI0_TWIM_vect_num, TWI0_TWIS_vect_num, '0');
	avr_spi_modern_init(avr, &mcu->spi0, 0x08c0, SPI0_INT_vect_num, '0');

	/* RTC + PIT at 0x0140. */
	avr_rtc_init(avr, &mcu->rtc, 0x0140, RTC_CNT_vect_num, RTC_PIT_vect_num, '0');

	/* ADC0 at 0x0600: RESRDY + WCOMP; temp-sensor cal from SIGROW. */
	avr_adc_modern_init(avr, &mcu->adc0, 0x0600,
						ADC0_RESRDY_vect_num, ADC0_WCOMP_vect_num, '0');
	avr_adc_modern_set_tempsense(&mcu->adc0, SIGROW_TEMPSENSE0);

	/* AC0 (analog comparator) at 0x0680. */
	avr_ac_init(avr, &mcu->ac0, 0x0680, AC0_AC_vect_num, '0');

	/* CCL at 0x01C0: 4 LUTs on megaAVR-0. */
	avr_ccl_init_mega(avr, &mcu->ccl, 0x01c0, 4, '0');

	/* AC0 OUT feeds the CCL input-source MUX (megaAVR-0 INSEL 0x6,
	 * DS40002173C p.373); megaAVR-0 fits only AC0 (no TCD). */
	avr_connect_irq(
			avr_io_getirq(avr, AVR_IOCTL_AC_GETIRQ('0'), AVR_AC_IRQ_OUT),
			avr_io_getirq(avr, AVR_IOCTL_CCL_GETIRQ('0'),
						  AVR_CCL_IRQ_SRC_4LUT(AVR_CCL_SRC_AC0)));

	/* TCA0 WO0/WO1/WO2 feed CCL INSEL 0xA (DS40002173C p.373: WOn on INn),
	 * so a single-slope PWM channel can drive a LUT directly. */
	for (int wo = 0; wo < 3; wo++)
		avr_connect_irq(
				avr_io_getirq(avr, AVR_IOCTL_TCA_GETIRQ('0'), AVR_TCA_IRQ_WO0 + wo),
				avr_io_getirq(avr, AVR_IOCTL_CCL_GETIRQ('0'),
							  AVR_CCL_IRQ_SRC_4LUT(AVR_CCL_SRC_TCA0_WO0 + wo)));

	/* TCB0/1/2 WO feed CCL INSEL 0xC (DS40002173C p.373: WO on IN0/1/2). The
	 * CCL reaches only TCB0-2 even on the 4-TCB parts. The TCB emits its
	 * 8-bit-PWM waveform level on this IRQ. */
	{
		static const char tcb_name[3] = { '0', '1', '2' };
		for (int i = 0; i < 3; i++)
			avr_connect_irq(
					avr_io_getirq(avr, AVR_IOCTL_TCB_GETIRQ(tcb_name[i]), AVR_TCB_IRQ_WO),
					avr_io_getirq(avr, AVR_IOCTL_CCL_GETIRQ('0'),
								  AVR_CCL_IRQ_SRC_4LUT(AVR_CCL_SRC_TCB0 + i)));
	}

	/* EVSYS routing fabric at 0x0180 (megaAVR-0 register layout). */
	avr_evsys_init_mega(avr, &mcu->evsys, 0x0180, '0');
	avr_irq_register_notify(
			avr_io_getirq(avr, AVR_IOCTL_AC_GETIRQ('0'), AVR_AC_IRQ_OUT),
			megax08_ac0_to_evsys, mcu);
	avr_irq_register_notify(
			avr_io_getirq(avr, AVR_IOCTL_EVSYS_GETIRQ('0'),
						  AVR_EVSYS_IRQ_USER0 + 8 /* USERADC0 */),
			megax08_evsys_to_adc, mcu);
	avr_irq_register_notify(
			avr_io_getirq(avr, AVR_IOCTL_TCB_GETIRQ('0'), AVR_TCB_IRQ_CAPT_OUT),
			megax08_tcb0_capt_to_evsys, mcu);
	avr_irq_register_notify(
			avr_io_getirq(avr, AVR_IOCTL_TCB_GETIRQ('1'), AVR_TCB_IRQ_CAPT_OUT),
			megax08_tcb1_capt_to_evsys, mcu);
	avr_irq_register_notify(
			avr_io_getirq(avr, AVR_IOCTL_TCB_GETIRQ('2'), AVR_TCB_IRQ_CAPT_OUT),
			megax08_tcb2_capt_to_evsys, mcu);
	/* megaAVR-0 EVSYS user order (iom4809.h EVSYS_t): CCLLUT0A..3B = 0..7,
	 * USERADC0 = 8, EVOUTA..F = 9..14, USERUSART0..3 = 15..18,
	 * USERTCA0 = 19, USERTCB0..3 = 20..23. */
	avr_irq_register_notify(
			avr_io_getirq(avr, AVR_IOCTL_EVSYS_GETIRQ('0'),
						  AVR_EVSYS_IRQ_USER0 + 20 /* USERTCB0 */),
			megax08_evsys_to_tcb0, mcu);
	avr_irq_register_notify(
			avr_io_getirq(avr, AVR_IOCTL_EVSYS_GETIRQ('0'),
						  AVR_EVSYS_IRQ_USER0 + 21 /* USERTCB1 */),
			megax08_evsys_to_tcb1, mcu);
	avr_irq_register_notify(
			avr_io_getirq(avr, AVR_IOCTL_EVSYS_GETIRQ('0'),
						  AVR_EVSYS_IRQ_USER0 + 22 /* USERTCB2 */),
			megax08_evsys_to_tcb2, mcu);
#ifdef TCB3_INT_vect_num
	avr_irq_register_notify(
			avr_io_getirq(avr, AVR_IOCTL_TCB_GETIRQ('3'), AVR_TCB_IRQ_CAPT_OUT),
			megax08_tcb3_capt_to_evsys, mcu);
	avr_irq_register_notify(
			avr_io_getirq(avr, AVR_IOCTL_EVSYS_GETIRQ('0'),
						  AVR_EVSYS_IRQ_USER0 + 23 /* USERTCB3 */),
			megax08_evsys_to_tcb3, mcu);
#endif
	/* EVSYS USERTCA0 (index 19) = TCA0 event input (EVCTRL.CNTEI). */
	avr_connect_irq(
			avr_io_getirq(avr, AVR_IOCTL_EVSYS_GETIRQ('0'),
						  AVR_EVSYS_IRQ_USER0 + 19 /* USERTCA0 */),
			avr_io_getirq(avr, AVR_IOCTL_TCA_GETIRQ('0'), AVR_TCA_IRQ_EV_IN));

	/* PORTMUX (peripheral pin routing) config store at 0x05E0. */
	avr_portmux_init(avr, &mcu->portmux, 0x05e0, '0');

	/* VREF at 0x00A0; its ADC0 reference feeds ADC0 (INTREF). */
	avr_vref_init(avr, &mcu->vref, 0x00a0, '0');
	avr_irq_register_notify(
			avr_io_getirq(avr, AVR_IOCTL_VREF_GETIRQ('0'), AVR_VREF_IRQ_ADC0_MV),
			megax08_vref_to_adc, mcu);

	/* WDT at 0x0100. */
	avr_wdt_modern_init(avr, &mcu->wdt, 0x0100, '0');
	avr_wdt_modern_set_reset_handler(&mcu->wdt, megax08_wdt_reset, mcu);

	/* CRCSCAN at 0x0120; APPEND/BOOTEND fuses (indices 7/8) bound the sections,
	 * CRC failure raises the NMI. */
	avr_crcscan_init(avr, &mcu->crcscan, 0x0120, FLASHEND + 1, 5, 7, 8,
					 CRCSCAN_NMI_vect_num, '0');

	/* SLPCTRL / RSTCTRL. */
	avr_slpctrl_init(avr, &mcu->slpctrl, 0x0050, '0');

	/* CPUINT (interrupt controller registers) at 0x0110. */
	avr_cpuint_init(avr, &mcu->cpuint, 0x0110, '0');
	avr_rstctrl_init(avr, &mcu->rstctrl, 0x0040, '0');

	/* BOD / VLM at 0x0080; CTRLA/B from FUSE.BODCFG (fuse index 1). */
	avr_bod_init(avr, &mcu->bod, 0x0080, BOD_VLM_vect_num, 1, '0');
	avr_bod_set_brownout_handler(&mcu->bod, megax08_bod_brownout, mcu);

	/* SYSCFG (REVID/EXTBRK) at 0x0F00 and SIGROW at 0x1100; revision A. */
	avr_syscfg_init(avr, &mcu->syscfg, 0x0f00, 0x1100, 0x00);
}

static void
megax08_reset(struct avr_t * avr)
{
	(void)avr;
}

const struct mcu_t SIM_CORENAME = {
	.core = {
		.mmcu = SIM_MMCU,
		MODERN_CORE(SIM_VECTOR_SIZE),

		.init = megax08_init,
		.reset = megax08_reset,
	},
};

/* The kind descriptor is declared in each cores/sim_megaNNN.c using this helper. */
static avr_t *
sim_megax08_make(void)
{
	return avr_core_allocate(&SIM_CORENAME.core, sizeof(struct mcu_t));
}
