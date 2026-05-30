/*
	test_attiny3217_blink.c

	Host-side test driving attiny3217_blink.axf: loads the firmware on the
	attiny3217 core, watches the PA3 pin IRQ, and asserts it toggles. This is
	the end-to-end "boots & blinks" check for the modern-AVR support.
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
	if (elf_read_firmware("attiny3217_blink.axf", &f)) {
		printf("FAIL: cannot read attiny3217_blink.axf\n");
		return 1;
	}
	avr_t *avr = avr_make_mcu_by_name(f.mmcu[0] ? f.mmcu : "attiny3217");
	if (!avr) { printf("FAIL: core attiny3217 not found\n"); return 1; }
	avr->log = LOG_ERROR;
	avr_init(avr);
	avr_load_firmware(avr, &f);

	avr_irq_t *porta = NULL;
	avr_ioctl(avr, AVR_IOCTL_PORT_GETIRQ('A'), &porta);
	if (!porta) { printf("FAIL: no PORTA IRQ\n"); return 1; }
	avr_irq_register_notify(porta + AVR_PORT_IRQ_PIN3, on_pa3, NULL);

	for (int i = 0; i < 200000 &&
			avr->state != cpu_Done && avr->state != cpu_Crashed; i++)
		avr_run(avr);

	int ok = (avr->state != cpu_Crashed) && (toggles > 10) &&
			 (avr->data[0x400] & (1 << 3)) /* DIR PA3 set */;
	printf("attiny3217 blink: state=%d PA3 toggles=%d DIR=%02x OUT=%02x -> %s\n",
			avr->state, toggles, avr->data[0x400], avr->data[0x404],
			ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}
