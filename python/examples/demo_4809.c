/*
 * GUI demo for ATmega4809: exercises pins, UART, a timer interrupt and a
 * breakpoint-friendly main loop.
 *   PA0  toggled by main() once per "hello" line
 *   PA1  toggled by the TCA0 overflow interrupt
 *   USART0 TX  prints "hello N" lines
 *
 *   avr-gcc -mmcu=atmega4809 -Os -g -I../../simavr/sim/avr -o demo_4809.elf demo_4809.c
 */
#include <avr/io.h>
#include <avr/interrupt.h>
#include <stdint.h>
#include "avr_mcu_section.h"

AVR_MCU(3333333, "atmega4809");

static volatile uint16_t ticks;

ISR(TCA0_OVF_vect)
{
    TCA0.SINGLE.INTFLAGS = TCA_SINGLE_OVF_bm;   /* clear the flag */
    PORTA.OUTTGL = PIN1_bm;
    ticks++;
}

static void put(char c)
{
    while (!(USART0.STATUS & USART_DREIF_bm))
        ;
    USART0.TXDATAL = c;
}

static void print_num(uint16_t n)
{
    char buf[5];
    uint8_t i = 0;
    do { buf[i++] = '0' + n % 10; n /= 10; } while (n);
    while (i)
        put(buf[--i]);
}

int main(void)
{
    PORTA.DIRSET = PIN0_bm | PIN1_bm;
    USART0.BAUD = 1;
    USART0.CTRLB = USART_TXEN_bm;

    TCA0.SINGLE.PER = 3000;
    TCA0.SINGLE.INTCTRL = TCA_SINGLE_OVF_bm;
    TCA0.SINGLE.CTRLA = TCA_SINGLE_ENABLE_bm;
    sei();

    for (uint16_t n = 0;; n++) {
        PORTA.OUTTGL = PIN0_bm;
        for (const char *s = "hello "; *s; s++)
            put(*s);
        print_num(n);
        put(' ');
        print_num(ticks);
        put('\n');
        for (volatile uint16_t d = 0; d < 2000; d++)
            ;
    }
}
