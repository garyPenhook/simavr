/*
	avr_rtc.h

	"Modern" AVR (AVRxt) Real-Time Counter (RTC) + Periodic Interrupt Timer
	(PIT), as found on the tinyAVR 1-series (ATtiny3217), megaAVR-0 and AVR Dx
	families.

	The block hosts two independent functions sharing one clock source
	(CLKSEL: internal 32.768 kHz / 1.024 kHz, 32 kHz crystal, or external):

	  * RTC counter — a 16-bit prescaled up-counter (0..PER) raising the single
	    RTC_CNT interrupt on overflow (OVF) and/or compare match (CMP).
	  * PIT — a periodic interrupt firing every 2^n RTC-clock cycles, on its own
	    RTC_PIT vector, independent of the RTC prescaler.

	Both are driven by simavr cycle timers. Because the RTC clock (~32 kHz) is
	decoupled from CLK_PER, periods are converted to CPU cycles via
	avr->frequency captured when the function is (re)started.

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

#ifndef __AVR_RTC_H__
#define __AVR_RTC_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "sim_avr.h"

/* Register offsets within an RTC block (device header RTC_t). */
enum {
	RTCR_CTRLA = 0x00,
	RTCR_STATUS = 0x01,
	RTCR_INTCTRL = 0x02,
	RTCR_INTFLAGS = 0x03,
	RTCR_TEMP = 0x04,
	RTCR_DBGCTRL = 0x05,
	RTCR_CLKSEL = 0x07,
	RTCR_CNTL = 0x08,
	RTCR_CNTH = 0x09,
	RTCR_PERL = 0x0a,
	RTCR_PERH = 0x0b,
	RTCR_CMPL = 0x0c,
	RTCR_CMPH = 0x0d,
	RTCR_PITCTRLA = 0x10,
	RTCR_PITSTATUS = 0x11,
	RTCR_PITINTCTRL = 0x12,
	RTCR_PITINTFLAGS = 0x13,
	RTCR_PITDBGCTRL = 0x15,
};

typedef struct avr_rtc_t {
	avr_io_t	io;
	char		name;

	avr_io_addr_t	base;
	avr_io_addr_t	r_ctrla, r_intctrl, r_intflags, r_clksel;
	avr_io_addr_t	r_cnt, r_per, r_cmp;	/* 16-bit (low byte address) */
	avr_io_addr_t	r_pitctrla, r_pitintctrl, r_pitintflags;

	avr_int_vector_t	cnt_vect;	/* RTC_CNT (OVF + CMP share this vector) */
	avr_int_vector_t	pit_vect;	/* RTC_PIT */

	/* RTC counter scheduler bookkeeping. */
	avr_cycle_count_t	cnt_start;	/* cycle at which CNT == 0 */
	uint32_t		cnt_cpt;	/* CPU cycles per RTC counter tick */
	uint32_t		ev_target;	/* next scheduled count */

	/* PIT scheduler bookkeeping. */
	avr_cycle_count_t	pit_start;	/* cycle the current PIT period began */
	uint32_t		pit_cpc;	/* CPU cycles per PIT period */
} avr_rtc_t;

/*
 * Initialise an RTC block at data address 'base'. 'vec_cnt' is the RTC_CNT
 * (overflow / compare) vector, 'vec_pit' the RTC_PIT vector; 'name' is a tag
 * for debug.
 */
void
avr_rtc_init(
		avr_t * avr,
		avr_rtc_t * p,
		avr_io_addr_t base,
		uint8_t vec_cnt,
		uint8_t vec_pit,
		char name);

#ifdef __cplusplus
};
#endif

#endif /* __AVR_RTC_H__ */
