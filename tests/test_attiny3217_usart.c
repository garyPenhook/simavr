/*
	test_attiny3217_usart.c

	Host-side test for attiny3217_usart.axf: capture the USART0 OUTPUT IRQ bytes
	and check the transmitted string.
 */
#include <stdio.h>
#include <string.h>
#include "sim_avr.h"
#include "sim_elf.h"
#include "sim_io.h"
#include "avr_usart.h"

static char rx[128];
static int  rxn;
static void on_tx(struct avr_irq_t *irq, uint32_t v, void *param)
{
	if (rxn < (int)sizeof(rx) - 1)
		rx[rxn++] = (char)v;
}

int main(void)
{
	const char *expect = "Hello modern AVR!\n";
	elf_firmware_t f;
	memset(&f, 0, sizeof(f));
	if (elf_read_firmware("attiny3217_usart.axf", &f)) {
		printf("FAIL: cannot read attiny3217_usart.axf\n");
		return 1;
	}
	avr_t *avr = avr_make_mcu_by_name(f.mmcu[0] ? f.mmcu : "attiny3217");
	if (!avr) { printf("FAIL: core not found\n"); return 1; }
	avr->log = LOG_ERROR;
	avr_init(avr);
	avr_load_firmware(avr, &f);

	avr_irq_t *u = NULL;
	avr_ioctl(avr, AVR_IOCTL_USART_GETIRQ('0'), &u);
	if (!u) { printf("FAIL: no USART0 IRQ\n"); return 1; }
	avr_irq_register_notify(u + AVR_USART_IRQ_OUTPUT, on_tx, NULL);

	for (int i = 0; i < 2000000 && rxn < (int)strlen(expect) &&
			avr->state != cpu_Crashed; i++)
		avr_run(avr);

	rx[rxn] = 0;
	int ok = (avr->state != cpu_Crashed) && (strcmp(rx, expect) == 0);
	printf("attiny3217 USART: got \"%s\" (%d bytes) -> %s\n",
			rx, rxn, ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}
