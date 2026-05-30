/*
	attiny3217_usart.c

	Modern-AVR (ATtiny3217) firmware: enable USART0 TX and send a string using
	polled DREIF. Exercises the modern USART peripheral. Driven by
	test_attiny3217_usart.c, which captures the emitted bytes.
 */
#include <avr/io.h>
#include "avr_mcu_section.h"

AVR_MCU(20000000, "attiny3217");

static void tx(char c)
{
	while (!(USART0.STATUS & USART_DREIF_bm))
		;
	USART0.TXDATAL = c;
}

int main(void)
{
	USART0.CTRLB = USART_TXEN_bm;
	const char *s = "Hello modern AVR!\n";
	for (const char *p = s; *p; p++)
		tx(*p);
	for (;;)
		;
}
