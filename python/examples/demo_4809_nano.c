/*
 * GUI demo for the ATmega4809 Curiosity Nano (boards/curiosity_nano_4809.json).
 *   PF5  LED0, active low: blinks at 1 Hz from the TCA0 overflow interrupt,
 *        held on while SW0 is pressed
 *   PF6  SW0, active low, internal pull-up (the board has no external one)
 *   USART0 TX  prints "tick N sw=0|1" once per blink phase
 *
 *   avr-gcc -mmcu=atmega4809 -Os -g -I../../simavr/sim/avr -o demo_4809_nano.elf demo_4809_nano.c
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
    if (PORTF.IN & PIN6_bm)
        PORTF.OUTTGL = PIN5_bm;                 /* released: blink */
    else
        PORTF.OUTCLR = PIN5_bm;                 /* pressed: LED on (active low) */
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
    PORTF.OUTSET = PIN5_bm;                     /* LED off before it becomes an output */
    PORTF.DIRSET = PIN5_bm;
    PORTF.PIN6CTRL = PORT_PULLUPEN_bm;          /* SW0 */
    PORTA.DIRSET = PIN0_bm;                     /* USART0 TX (default pins) */
    USART0.BAUD = 1;
    USART0.CTRLB = USART_TXEN_bm;

    TCA0.SINGLE.PER = 26041;                    /* 3.333 MHz / 64 / 26042 ~ 2 Hz */
    TCA0.SINGLE.INTCTRL = TCA_SINGLE_OVF_bm;
    TCA0.SINGLE.CTRLA = TCA_SINGLE_CLKSEL_DIV64_gc | TCA_SINGLE_ENABLE_bm;
    sei();

    for (uint16_t seen = 0;; ) {
        uint16_t t;
        cli(); t = ticks; sei();                /* 16-bit read vs the ISR */
        if (t == seen)
            continue;
        seen = t;
        for (const char *s = "tick "; *s; s++)
            put(*s);
        print_num(t);
        for (const char *s = " sw="; *s; s++)
            put(*s);
        put((PORTF.IN & PIN6_bm) ? '1' : '0');
        put('\n');
    }
}
