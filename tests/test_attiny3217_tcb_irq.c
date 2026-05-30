/*
	test_attiny3217_tcb_irq.c

	Host-side test for attiny3217_tcb_irq.axf: the only way PA3 toggles is from
	the TCB0 ISR, so observing toggles proves the CPUINT dispatch + TCB timer
	chain works end to end on the modern core.
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
	if (elf_read_firmware("attiny3217_tcb_irq.axf", &f)) {
		printf("FAIL: cannot read attiny3217_tcb_irq.axf\n");
		return 1;
	}
	avr_t *avr = avr_make_mcu_by_name(f.mmcu[0] ? f.mmcu : "attiny3217");
	if (!avr) { printf("FAIL: core not found\n"); return 1; }
	avr->log = LOG_ERROR;
	avr_init(avr);
	avr_load_firmware(avr, &f);

	avr_irq_t *porta = NULL;
	avr_ioctl(avr, AVR_IOCTL_PORT_GETIRQ('A'), &porta);
	if (!porta) { printf("FAIL: no PORTA IRQ\n"); return 1; }
	avr_irq_register_notify(porta + AVR_PORT_IRQ_PIN3, on_pa3, NULL);

	for (int i = 0; i < 500000 &&
			avr->state != cpu_Done && avr->state != cpu_Crashed; i++)
		avr_run(avr);

	int ok = (avr->state != cpu_Crashed) && (toggles > 20);
	printf("attiny3217 TCB irq: state=%d PA3 toggles=%d -> %s\n",
			avr->state, toggles, ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}
