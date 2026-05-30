/*
	test_attiny3217_adc.c

	Host-side test: present 2500 mV on ADC AIN3 (half of the 5000 mV reference)
	and confirm the firmware reads ~512 (10-bit half-scale) and raises PA3.
 */
#include <stdio.h>
#include <string.h>
#include "sim_avr.h"
#include "sim_elf.h"
#include "sim_io.h"
#include "avr_port.h"
#include "avr_adc_modern.h"

static int pa3_high;
static void on_pa3(struct avr_irq_t *i, uint32_t v, void *p){ if(v) pa3_high = 1; }

int main(void)
{
	elf_firmware_t f;
	memset(&f, 0, sizeof(f));
	if (elf_read_firmware("attiny3217_adc.axf", &f)) { printf("FAIL: read\n"); return 1; }
	avr_t *avr = avr_make_mcu_by_name(f.mmcu[0] ? f.mmcu : "attiny3217");
	if (!avr) { printf("FAIL: core\n"); return 1; }
	avr->log = LOG_ERROR;
	avr_init(avr);
	avr_load_firmware(avr, &f);

	avr_irq_t *adc = NULL;
	avr_ioctl(avr, AVR_IOCTL_ADCM_GETIRQ('0'), &adc);
	if (!adc) { printf("FAIL: no ADC IRQ\n"); return 1; }
	avr_raise_irq(adc + AVR_ADCM_IRQ_AIN0 + 3, 2500);	// AIN3 = 2500 mV

	avr_irq_t *pa = NULL;
	avr_ioctl(avr, AVR_IOCTL_PORT_GETIRQ('A'), &pa);
	avr_irq_register_notify(pa + AVR_PORT_IRQ_PIN3, on_pa3, NULL);

	for (int i = 0; i < 300000 && !pa3_high && avr->state != cpu_Crashed; i++)
		avr_run(avr);

	int ok = pa3_high && avr->state != cpu_Crashed;
	printf("attiny3217 ADC: 2500mV/5000mV -> ~512 %s -> %s\n",
			pa3_high ? "OK" : "FAILED", ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}
