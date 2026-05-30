/*
	attiny3217_spi.c

	Modern-AVR (ATtiny3217) firmware: SPI0 master, transfer two bytes and check
	the received (looped-back) values, signalling success on PA3.
 */
#include <avr/io.h>
#include "avr_mcu_section.h"

AVR_MCU(20000000, "attiny3217");

static uint8_t spi_xfer(uint8_t b)
{
	SPI0.DATA = b;
	while (!(SPI0.INTFLAGS & SPI_IF_bm))
		;
	return SPI0.DATA;
}

int main(void)
{
	PORTA.DIRSET = (1 << 3);
	SPI0.CTRLA = SPI_ENABLE_bm | SPI_MASTER_bm;
	uint8_t r1 = spi_xfer(0x5A);
	uint8_t r2 = spi_xfer(0xC3);
	if (r1 == 0x5A && r2 == 0xC3)
		PORTA.OUTSET = (1 << 3);	// success (loopback echoed)
	for (;;)
		;
}
