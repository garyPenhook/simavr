/*
	attiny3217_eeprom.c

	Modern-AVR (ATtiny3217) firmware: write two EEPROM bytes through the mapped
	region + NVMCTRL command, read them back, and signal success on PA3.
	Exercises the NVMCTRL EEPROM path.
 */
#include <avr/io.h>
#include "avr_mcu_section.h"

AVR_MCU(20000000, "attiny3217");

static void ee_write(uint16_t off, uint8_t val)
{
	while (NVMCTRL.STATUS & NVMCTRL_EEBUSY_bm)
		;
	*(volatile uint8_t *)(MAPPED_EEPROM_START + off) = val;	// load page buffer
	_PROTECTED_WRITE(NVMCTRL.CTRLA, NVMCTRL_CMD_PAGEERASEWRITE_gc);	// commit
}

static uint8_t ee_read(uint16_t off)
{
	return *(volatile uint8_t *)(MAPPED_EEPROM_START + off);
}

int main(void)
{
	PORTA.DIRSET = (1 << 3);
	ee_write(10, 0x42);
	ee_write(11, 0xA5);
	if (ee_read(10) == 0x42 && ee_read(11) == 0xA5)
		PORTA.OUTSET = (1 << 3);	// success -> PA3 high
	for (;;)
		;
}
