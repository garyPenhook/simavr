/*
 * GUI demo for ATmega4809: exercises pins, UART, a timer interrupt and a
 * breakpoint-friendly main loop.
 *   PA0  toggled by main() once per "hello" line
 *   PA1  toggled by the TCA0 overflow interrupt
 *   USART0 TX  prints "hello N" lines
 *   PD0/PD1/PD4-7  HD44780 LCD, 4-bit (RS, E, D4-D7), as in boards/demo_4809.json
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

static void lcd_nib(uint8_t n, uint8_t rs)
{
    PORTD.OUT = (n << 4) | rs;
    PORTD.OUT = (n << 4) | rs | 2;              /* E high */
    PORTD.OUT = (n << 4) | rs;                  /* E falling edge latches */
}

static void lcd_send(uint8_t b, uint8_t rs)
{
    lcd_nib(b >> 4, rs);
    lcd_nib(b & 15, rs);
}

static void lcd_str(const char *s)
{
    while (*s)
        lcd_send(*s++, 1);
}

static void lcd_num(uint16_t n)
{
    char buf[5];
    uint8_t i = 0;
    do { buf[i++] = '0' + n % 10; n /= 10; } while (n);
    while (i)
        lcd_send(buf[--i], 1);
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
    PORTD.DIRSET = 0xF3;
    lcd_nib(3, 0); lcd_nib(3, 0); lcd_nib(3, 0); lcd_nib(2, 0);     /* 4-bit init */
    lcd_send(0x28, 0);                          /* 2 lines, 5x8 font */
    lcd_send(0x0C, 0);                          /* display on, no cursor */
    lcd_send(0x06, 0);                          /* entry mode: increment */
    lcd_send(0x01, 0);                          /* clear */
    lcd_str("simavr demo");
    USART0.BAUD = 1;
    USART0.CTRLB = USART_TXEN_bm;

    TCA0.SINGLE.PER = 3000;
    TCA0.SINGLE.INTCTRL = TCA_SINGLE_OVF_bm;
    TCA0.SINGLE.CTRLA = TCA_SINGLE_ENABLE_bm;
    sei();

    for (uint16_t n = 0;; n++) {
        PORTA.OUTTGL = PIN0_bm;
        lcd_send(0xC0, 0);                      /* line 2 */
        lcd_str("count ");
        lcd_num(n);
        lcd_str("   ");
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
