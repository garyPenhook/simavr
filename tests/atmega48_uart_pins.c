#ifndef F_CPU
#define F_CPU 8000000
#endif
#include <avr/io.h>
#include <stdio.h>
#include <avr/interrupt.h>
#include <avr/sleep.h>

/*
 * Coverage for the classic USART TxD/RxD pin-function override (avr_uart.c).
 * When TXEN is set the USART owns the TxD pin (PD1) as a high-idle output,
 * overriding the GPIO DDR/PORT registers; clearing TXEN returns it to GPIO.
 *
 * Pull-ups are disabled so a GPIO input reads 0 when undriven, making the
 * USART-forced high level unambiguous.
 */
#include "avr_mcu_section.h"
AVR_MCU(F_CPU, "atmega48");
AVR_MCU_PORT_NO_PULL;

static int uart_putchar(char c, FILE *stream) {
	if (c == '\n')
		uart_putchar('\r', stream);
	loop_until_bit_is_set(UCSR0A, UDRE0);
	UDR0 = c;
	return 0;
}

static FILE mystdout = FDEV_SETUP_STREAM(uart_putchar, NULL,
                                         _FDEV_SETUP_WRITE);

#define TXD_BIT (1 << PD1)
#define txd_level() ((PIND & TXD_BIT) ? 1 : 0)

int main(void)
{
	stdout = &mystdout;

	/* TxD is a plain GPIO input for now: undriven, pull-ups off -> reads 0. */
	DDRD = 0;
	PORTD = 0;
	uint8_t before = txd_level();			/* expect 0 */

	/* Enable the transmitter: the USART now owns TxD as a high output. */
	UCSR0B = _BV(TXEN0);
	uint8_t claimed = txd_level();			/* expect 1 */

	/* GPIO tries to take the pin back (drive it low, make it an output):
	 * the USART owns it, so the level stays high. */
	PORTD = 0;
	DDRD = TXD_BIT;
	uint8_t owned = txd_level();			/* expect 1 */

	/* Disable the transmitter: TxD returns to GPIO (input, undriven -> 0). */
	UCSR0B = 0;
	DDRD = 0;
	PORTD = 0;
	uint8_t released = txd_level();			/* expect 0 */

	/* Re-enable TX so the UART can transmit the result line. */
	UCSR0B = _BV(TXEN0);
	printf("T<%X%X%X%X ", before, claimed, owned, released);	/* T<0110 */

	cli();
	sleep_cpu();
}
