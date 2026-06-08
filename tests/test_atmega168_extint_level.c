#include <stdio.h>
#include <string.h>
#include "tests.h"

/*
 * Drives the level-triggered (low-level) external-interrupt firmware. The
 * firmware self-triggers INT0 by driving its own PD2 output low, so this
 * harness only needs to check the reported counts:
 *   L<03  continuous low-level triggers, stopped by the ISR after 3
 *   L<03  no runaway: the count held after the level was released
 *   L<00  re-asserting the level with INT0 masked produces no interrupt
 */

int main(int argc, char **argv) {
	avr_t *avr;
	static const char *expected = "L<03 L<03 L<00 ";

	tests_init(argc, argv);
	avr = tests_init_avr("atmega168_extint_level.axf");

	tests_assert_uart_receive_avr(avr, 1000000, expected, '0');
	tests_success();
	return 0;
}
