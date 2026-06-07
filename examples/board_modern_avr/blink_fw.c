/*
	blink_fw.c

	A *device-agnostic* modern-AVR (AVRxt) peripheral self-test firmware. The
	same source is compiled once per supported modern core (all 15 tinyAVR
	1-series + 8 megaAVR 0-series) by this board's Makefile, each build passing
	the device name in MCU_NAME so the embedded .mmcu section names the right
	core for simavr.

	It touches only peripherals that exist identically on every modern AVR, from
	the 8-pin ATtiny212 up to the 48-pin ATmega4809, and reports a result bitmask
	(one bit per peripheral that responded) on USART0 every loop:

	  bit 0  PORT   PA0 toggles                       (the "LED")
	  bit 1  TCA0   16-bit timer counts
	  bit 2  TCB0   timer/counter B counts
	  bit 3  RTC    real-time counter ticks (OSCULP32K)
	  bit 4  ADC0   conversion completes (RESRDY)
	  bit 5  SPI0   master byte shifts out (IF)
	  bit 6  TWI0   master addresses a slave and gets a result (WIF/RIF)

	USART0 is the transport, so it is implicitly covered too. Everything is
	driven through the avr-libc device-header peripheral structs (TCA0, TCB0,
	RTC, ADC0, SPI0, TWI0, USART0) — identical across both families — and uses
	bounded waits so a non-responding model can never hang the run. The host
	(runner.c) ORs the received bytes and PASSes a core only if every required
	bit appears; the bit-7-free byte also lets it print which peripherals ran.

	Anything that varies by device (physical pin routing via PORTMUX, extra
	ports / USART / TCB instances) is intentionally left out so one source builds
	and runs unchanged across the whole family; see board_atmega4809 /
	board_attiny3217 for richer per-family demos that drive the real pins.

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

#include <avr/io.h>
#include <util/delay.h>
#include <stdint.h>

// for linker, emulator, and programmer's sake
#include "avr_mcu_section.h"

#ifndef MCU_NAME
#error "MCU_NAME must be defined by the build (e.g. -DMCU_NAME='\"attiny3217\"')"
#endif

AVR_MCU(F_CPU, MCU_NAME);

/* Result bits, one per peripheral. Kept in sync with runner.c. */
#define R_PORT	(1 << 0)
#define R_TCA	(1 << 1)
#define R_TCB	(1 << 2)
#define R_RTC	(1 << 3)
#define R_ADC	(1 << 4)
#define R_SPI	(1 << 5)
#define R_TWI	(1 << 6)

/* 9600 baud; the exact value is irrelevant to the wire IRQ, only that TX runs. */
#define USART0_BAUD_RATE(br)	((uint16_t)((F_CPU * 64.0) / (16.0 * (br)) + 0.5))

/* Spin until *reg masks 'mask' or the bounded budget runs out; never hangs. */
static uint8_t
wait_flag(volatile uint8_t *reg, uint8_t mask, uint16_t budget)
{
	while (budget-- && !(*reg & mask))
		;
	return (*reg & mask) != 0;
}

static void
usart0_tx(uint8_t b)
{
	while (!(USART0.STATUS & USART_DREIF_bm))
		;
	USART0.TXDATAL = b;
}

int
main(void)
{
	/* PORT: PA0 as output (the "LED"). */
	PORTA.DIRSET = PIN0_bm;

	/* USART0 TX (the result transport). */
	USART0.BAUD = USART0_BAUD_RATE(9600);
	USART0.CTRLB = USART_TXEN_bm;

	/* TCA0: enable, count CLK_PER directly. */
	TCA0.SINGLE.PER = 0xFFFF;
	TCA0.SINGLE.CTRLA = TCA_SINGLE_ENABLE_bm;

	/* TCB0: enable, periodic (default CNTMODE), clocked from CLK_PER. */
	TCB0.CCMP = 0xFFFF;
	TCB0.CTRLA = TCB_ENABLE_bm;

	/* RTC: enable from the always-available 32 kHz ULP oscillator. */
	(void)wait_flag(&RTC.STATUS, RTC_CTRLABUSY_bm, 0);	/* read clears nothing; just be safe below */
	while (RTC.STATUS & RTC_CTRLABUSY_bm)
		;
	RTC.CTRLA = RTC_RTCEN_bm;

	/* ADC0: enable, VDD reference, modest prescaler; convert AIN0. */
	ADC0.CTRLC = ADC_PRESC_DIV4_gc | ADC_REFSEL_VDDREF_gc;
	ADC0.MUXPOS = 0;
	ADC0.CTRLA = ADC_ENABLE_bm;

	/* SPI0: master; SS-disable so a stray SS pin can't clear master mode. */
	SPI0.CTRLB = SPI_SSD_bm;
	SPI0.CTRLA = SPI_ENABLE_bm | SPI_MASTER_bm;

	/* TWI0: master; force the bus to idle so the first transaction can start. */
	TWI0.MBAUD = 10;
	TWI0.MCTRLA = TWI_ENABLE_bm;
	TWI0.MSTATUS = TWI_BUSSTATE_IDLE_gc;

	for (;;) {
		uint8_t r = 0;

		PORTA.OUTTGL = PIN0_bm;
		r |= R_PORT;

		if (TCA0.SINGLE.CNT)
			r |= R_TCA;
		if (TCB0.CNT)
			r |= R_TCB;
		if (RTC.CNT)
			r |= R_RTC;

		/* ADC: start a conversion, bounded wait for RESRDY, then clear it. */
		ADC0.COMMAND = ADC_STCONV_bm;
		if (wait_flag((volatile uint8_t *)&ADC0.INTFLAGS, ADC_RESRDY_bm, 4000)) {
			(void)ADC0.RES;
			ADC0.INTFLAGS = ADC_RESRDY_bm;
			r |= R_ADC;
		}

		/* SPI: clock a byte out as master, bounded wait for IF. */
		SPI0.DATA = 0xA5;
		if (wait_flag((volatile uint8_t *)&SPI0.INTFLAGS, SPI_IF_bm, 4000)) {
			(void)SPI0.DATA;	/* reading DATA clears IF */
			r |= R_SPI;
		}

		/* TWI: address slave 0x50 for a write; the host runner ACKs it. */
		TWI0.MADDR = (0x50 << 1) | 0;
		if (wait_flag((volatile uint8_t *)&TWI0.MSTATUS,
			      TWI_WIF_bm | TWI_RIF_bm, 4000)) {
			TWI0.MCTRLB = TWI_MCMD_STOP_gc;
			r |= R_TWI;
		}

		usart0_tx(r);
		_delay_ms(10);
	}
}
