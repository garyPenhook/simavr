#ifndef F_CPU
#define F_CPU 8000000
#endif
#include <avr/io.h>
#include <stdio.h>
#include <avr/interrupt.h>
#include <avr/sleep.h>

/*
 * Regression coverage for the classic low-level (level-triggered) external
 * interrupt path in avr_extint.c — the case the two ioport tests explicitly
 * skip with a TODO, and the one historically prone to "continuous interrupts".
 *
 * INT0 (PD2) is configured as an OUTPUT so the firmware itself can drive the
 * pin level: driving it low asserts the low-level interrupt, which then keeps
 * re-triggering for as long as the pin is held low. The ISR counts entries and
 * releases the level (drives PD2 high) after a few, proving both that the
 * level source re-triggers continuously AND that it stops once the level is
 * removed (no runaway interrupt).
 */
#include "avr_mcu_section.h"
AVR_MCU(F_CPU, "atmega168");

static int uart_putchar(char c, FILE *stream) {
	if (c == '\n')
		uart_putchar('\r', stream);
	loop_until_bit_is_set(UCSR0A, UDRE0);
	UDR0 = c;
	return 0;
}

static FILE mystdout = FDEV_SETUP_STREAM(uart_putchar, NULL,
                                         _FDEV_SETUP_WRITE);

#define RELEASE_AFTER 3

static volatile uint8_t int0_count;

ISR(INT0_vect)
{
	int0_count++;
	/* Release the low level after a few continuous triggers so the
	 * level-triggered source stops re-asserting and we do not loop forever. */
	if (int0_count >= RELEASE_AFTER)
		PORTD |= _BV(PD2);	/* drive PD2 (an output) high */
}

int main(void)
{
	stdout = &mystdout;

	/* INT0/PD2 as a high (inactive) output. */
	DDRD = _BV(PD2);
	PORTD = _BV(PD2);

	/* INT0 low-level triggered: EICRA ISC01:ISC00 = 00. */
	EICRA = 0;
	EIMSK = _BV(INT0);
	sei();

	/* Assert the low level: this transition triggers INT0, which then
	 * re-triggers continuously until the ISR drives PD2 high again. */
	PORTD &= ~_BV(PD2);

	while (int0_count < RELEASE_AFTER)
		;

	printf("L<%02X ", int0_count);		/* expect L<03 */

	/* The level was released inside the ISR; confirm no further triggers
	 * occur (the count must stay put). */
	for (volatile uint16_t i = 0; i < 4000; i++)
		;
	printf("L<%02X ", int0_count);		/* still L<03: stopped cleanly */

	/* Re-assert with the interrupt masked off: no interrupt may fire even
	 * though the pin is low (the level source must respect the mask). */
	cli();
	int0_count = 0;
	EIMSK = 0;
	PORTD &= ~_BV(PD2);			/* pin low again, but INT0 masked */
	sei();
	for (volatile uint16_t i = 0; i < 4000; i++)
		;
	printf("L<%02X ", int0_count);		/* expect L<00: masked, no trigger */

	/* this quits the simulator, since interrupts are off */
	cli();
	sleep_cpu();
}
