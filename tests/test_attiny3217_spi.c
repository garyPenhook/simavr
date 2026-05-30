/*
	test_attiny3217_spi.c

	Host-side test: wire SPI0 MOSI (OUTPUT) straight back to MISO (INPUT) so the
	master receives what it sent; the firmware raises PA3 only if the loopback
	round-trip matched.
 */
#include <stdio.h>
#include <string.h>
#include "sim_avr.h"
#include "sim_elf.h"
#include "sim_io.h"
#include "avr_port.h"
#include "avr_spi_modern.h"

static avr_irq_t *spi;
static int pa3_high;
static void on_mosi(struct avr_irq_t *i, uint32_t v, void *p)
{
	avr_raise_irq(spi + AVR_SPIM_IRQ_INPUT, v);	// loopback MOSI -> MISO
}
static void on_pa3(struct avr_irq_t *i, uint32_t v, void *p){ if(v) pa3_high = 1; }

int main(void)
{
	elf_firmware_t f;
	memset(&f, 0, sizeof(f));
	if (elf_read_firmware("attiny3217_spi.axf", &f)) { printf("FAIL: read\n"); return 1; }
	avr_t *avr = avr_make_mcu_by_name(f.mmcu[0] ? f.mmcu : "attiny3217");
	if (!avr) { printf("FAIL: core\n"); return 1; }
	avr->log = LOG_ERROR;
	avr_init(avr);
	avr_load_firmware(avr, &f);

	avr_ioctl(avr, AVR_IOCTL_SPIM_GETIRQ('0'), &spi);
	if (!spi) { printf("FAIL: no SPI IRQ\n"); return 1; }
	avr_irq_register_notify(spi + AVR_SPIM_IRQ_OUTPUT, on_mosi, NULL);
	avr_irq_t *pa = NULL;
	avr_ioctl(avr, AVR_IOCTL_PORT_GETIRQ('A'), &pa);
	avr_irq_register_notify(pa + AVR_PORT_IRQ_PIN3, on_pa3, NULL);

	for (int i = 0; i < 300000 && !pa3_high && avr->state != cpu_Crashed; i++)
		avr_run(avr);

	int ok = pa3_high && avr->state != cpu_Crashed;
	printf("attiny3217 SPI: loopback %s -> %s\n",
			pa3_high ? "OK" : "FAILED", ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}
