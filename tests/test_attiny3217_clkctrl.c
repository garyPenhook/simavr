/*
	test_attiny3217_clkctrl.c

	Host-side test: the firmware only reaches the blink loop after a
	CCP-protected MCLKCTRLB write and a successful MCLKSTATUS.OSC20MS poll, so
	observing PA3 toggles proves both work.
 */
#include <stdio.h>
#include <string.h>
#include "sim_avr.h"
#include "sim_elf.h"
#include "sim_io.h"
#include "avr_port.h"

static int toggles;
static uint8_t last = 2;
static void on_pa3(struct avr_irq_t *irq, uint32_t v, void *param)
{
	if (v != last) { toggles++; last = v; }
}

int main(void)
{
	elf_firmware_t f;
	memset(&f, 0, sizeof(f));
	if (elf_read_firmware("attiny3217_clkctrl.axf", &f)) {
		printf("FAIL: cannot read attiny3217_clkctrl.axf\n");
		return 1;
	}
	avr_t *avr = avr_make_mcu_by_name(f.mmcu[0] ? f.mmcu : "attiny3217");
	if (!avr) { printf("FAIL: core not found\n"); return 1; }
	avr->log = LOG_ERROR;
	avr_init(avr);
	avr_load_firmware(avr, &f);

	avr_irq_t *porta = NULL;
	avr_ioctl(avr, AVR_IOCTL_PORT_GETIRQ('A'), &porta);
	avr_irq_register_notify(porta + AVR_PORT_IRQ_PIN3, on_pa3, NULL);

	for (int i = 0; i < 200000 && avr->state != cpu_Crashed; i++)
		avr_run(avr);

	int ok = (avr->state != cpu_Crashed) && (toggles > 10);
	printf("attiny3217 CLKCTRL: PA3 toggles=%d (passed OSC20MS poll) -> %s\n",
			toggles, ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}
