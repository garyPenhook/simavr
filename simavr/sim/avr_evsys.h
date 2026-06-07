/*
	avr_evsys.h

	"Modern" AVR (AVRxt) Event System (EVSYS at 0x0180 on the tinyAVR 1-series,
	also megaAVR-0 and AVR Dx families).

	EVSYS routes events from generators onto channels, and from channels to the
	"users" (event-consuming peripherals). This models the routing fabric as an
	observable switch matrix:

	  * Six channels: ch0 = SYNCCH0, ch1 = SYNCCH1, ch2..5 = ASYNCCH0..3. Each
	    carries a level (0/1) that a generator or a test drives by raising the
	    matching CHn IRQ; the software strobe registers pulse a channel.
	  * Each user register (ASYNCUSER0..12, SYNCUSER0..1) selects a channel; the
	    select value v maps uniformly to channel v-1 (0 = off). When a routed
	    channel changes, the user's current value is emitted on its USERn OUT IRQ.

	Real generators drive channels through avr_evsys_async_generator(): the core
	connects a peripheral's event-source IRQ to it with the generator's select
	value, and EVSYS drives every async channel (ASYNCCH0..3) whose generator
	select register holds that value (the four async channels share one source
	encoding). A channel can still also be driven directly via its CHn IRQ or the
	software strobe. Channels deliver to the real user peripherals through the
	USERn OUT IRQs (e.g. sim_tiny3217 wires the ADC0 user to the ADC event start).

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

#ifndef __AVR_EVSYS_H__
#define __AVR_EVSYS_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "sim_avr.h"

#define AVR_EVSYS_MAX_CHANNELS	8
#define AVR_EVSYS_MAX_USERS		24

/* Register offsets within an EVSYS block (device header EVSYS_t). */
enum {
	EVSYSR_ASYNCSTROBE = 0x00,
	EVSYSR_SYNCSTROBE = 0x01,
	EVSYSR_ASYNCCH0 = 0x02,		/* ASYNCCH0..3 at 0x02..0x05 */
	EVSYSR_SYNCCH0 = 0x0a,		/* SYNCCH0..1 at 0x0a..0x0b */
	EVSYSR_ASYNCUSER0 = 0x12,	/* ASYNCUSER0..12 at 0x12..0x1e */
	EVSYSR_SYNCUSER0 = 0x22,	/* SYNCUSER0..1 at 0x22..0x23 */
};

/*
 * IRQs: CHn (index 0..nchannels-1) drive a channel's level; USERn
 * (index AVR_EVSYS_IRQ_USER0 + user) report the value a user receives.
 */
enum {
	AVR_EVSYS_IRQ_CH0 = 0,
	AVR_EVSYS_IRQ_USER0 = AVR_EVSYS_MAX_CHANNELS,
	AVR_EVSYS_IRQ_COUNT = AVR_EVSYS_MAX_CHANNELS + AVR_EVSYS_MAX_USERS,
};

typedef struct avr_evsys_t {
	avr_io_t	io;
	char		name;

	avr_io_addr_t	base;
	avr_io_addr_t	r_strobe[2];
	uint8_t		strobe_first_channel[2];
	uint8_t		strobe_width[2];
	uint8_t		nstrobe;
	avr_io_addr_t	r_user[AVR_EVSYS_MAX_USERS];
	avr_io_addr_t	r_chan[AVR_EVSYS_MAX_CHANNELS];
	uint8_t		nchannels;
	uint8_t		nusers;
	uint8_t		chan[AVR_EVSYS_MAX_CHANNELS];	/* current channel levels */
	int			base_irq;
} avr_evsys_t;

/* User indices into the USERn OUT IRQs (AVR_EVSYS_IRQ_USER0 + index). */
enum {
	AVR_EVSYS_USER_TCB0 = 0,
	AVR_EVSYS_USER_ADC0 = 1,
	AVR_EVSYS_USER_EVOUT0 = 8,
	AVR_EVSYS_USER_EVOUT1 = 9,
	AVR_EVSYS_USER_EVOUT2 = 10,
	AVR_EVSYS_USER_TCA0 = 13,
	AVR_EVSYS_USER_USART0 = 14,
};

/*
 * Initialise an EVSYS block at data address 'base'. 'name' is a tag for debug
 * and the IRQ ioctl.
 */
void
avr_evsys_init(
		avr_t * avr,
		avr_evsys_t * p,
		avr_io_addr_t base,
		char name);

void
avr_evsys_init_mega(
		avr_t * avr,
		avr_evsys_t * p,
		avr_io_addr_t base,
		char name);

/*
 * A generator fired: drive 'level' onto every configured channel whose
 * generator-select register holds 'gen_value', propagating to that channel's
 * users.
 */
void
avr_evsys_async_generator(avr_evsys_t * p, uint8_t gen_value, uint8_t level);

#define AVR_IOCTL_EVSYS_GETIRQ(_name) AVR_IOCTL_DEF('e','v','s',(_name))

#ifdef __cplusplus
};
#endif

#endif /* __AVR_EVSYS_H__ */
