#include <stdio.h>
#include <string.h>
#include "tests.h"

/*
 * Drives the USART TxD pin-function override firmware and checks the reported
 * TxD pin levels:
 *   0  TxD is a GPIO input (undriven, pull-ups off)
 *   1  TXEN set: USART owns TxD as a high output
 *   1  GPIO cannot pull it back while the USART owns it
 *   0  TXEN cleared: TxD returns to GPIO
 */

int main(int argc, char **argv) {
	avr_t *avr;
	static const char *expected = "T<0110 ";

	tests_init(argc, argv);
	avr = tests_init_avr("atmega48_uart_pins.axf");

	tests_assert_uart_receive_avr(avr, 1000000, expected, '0');
	tests_success();
	return 0;
}
