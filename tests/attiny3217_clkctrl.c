/*
	attiny3217_clkctrl.c

	Modern-AVR (ATtiny3217) firmware: do a CCP-protected clock-prescaler change,
	then wait for the OSC20M oscillator to report stable before blinking PA3.
	Without a CLKCTRL model the OSC20MS poll would spin forever, so this checks
	both CCP-protected writes and MCLKSTATUS.
 */
#include <avr/io.h>
#include "avr_mcu_section.h"

AVR_MCU(20000000, "attiny3217");

int main(void)
{
	/* disable the main-clock prescaler (CCP-protected register) */
	_PROTECTED_WRITE(CLKCTRL.MCLKCTRLB, 0);
	/* wait for the 20 MHz oscillator to be running and stable */
	while (!(CLKCTRL.MCLKSTATUS & CLKCTRL_OSC20MS_bm))
		;
	PORTA.DIRSET = (1 << 3);
	for (;;)
		PORTA.OUTTGL = (1 << 3);
}
