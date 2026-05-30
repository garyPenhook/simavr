/*
	test_attiny3217_tca.c

	Host-side test for attiny3217_tca.axf: PA3 toggles from the TCA0 overflow
	ISR and PA4 from the CMP0 ISR. Observing both proves the TCA counter,
	overflow and compare interrupts work through CPUINT.
 */
#include <stdio.h>
#include <string.h>
#include "sim_avr.h"
#include "sim_elf.h"
#include "sim_io.h"
#include "avr_port.h"

static int ovf_toggles, cmp_toggles;
static uint8_t l3 = 2, l4 = 2;
static void on_pa3(struct avr_irq_t *i, uint32_t v, void *p){ if(v!=l3){ovf_toggles++;l3=v;} }
static void on_pa4(struct avr_irq_t *i, uint32_t v, void *p){ if(v!=l4){cmp_toggles++;l4=v;} }

int main(void)
{
	elf_firmware_t f;
	memset(&f, 0, sizeof(f));
	if (elf_read_firmware("attiny3217_tca.axf", &f)) { printf("FAIL: read\n"); return 1; }
	avr_t *avr = avr_make_mcu_by_name(f.mmcu[0] ? f.mmcu : "attiny3217");
	if (!avr) { printf("FAIL: core\n"); return 1; }
	avr->log = LOG_ERROR;
	avr_init(avr);
	avr_load_firmware(avr, &f);
	avr_irq_t *pa = NULL;
	avr_ioctl(avr, AVR_IOCTL_PORT_GETIRQ('A'), &pa);
	avr_irq_register_notify(pa + AVR_PORT_IRQ_PIN3, on_pa3, NULL);
	avr_irq_register_notify(pa + AVR_PORT_IRQ_PIN4, on_pa4, NULL);

	for (int i = 0; i < 1000000 && avr->state != cpu_Crashed; i++)
		avr_run(avr);

	int ok = (avr->state != cpu_Crashed) && ovf_toggles > 20 && cmp_toggles > 20;
	printf("attiny3217 TCA: OVF toggles=%d CMP0 toggles=%d -> %s\n",
			ovf_toggles, cmp_toggles, ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}
