/*
 * GUI demo for the ATtiny13A (boards/demo_13a.json).
 *   PB0  LED0: blinks at ~1 Hz from the Timer0 overflow interrupt
 *   PB2  LED1: on while SW0 is pressed
 *   PB3  SW0, active low, internal pull-up
 *   ADC  one conversion of ADC2 (PB4) per blink, result kept in `sample`
 *
 *   avr-gcc -mmcu=attiny13a -Os -g -I../../simavr/sim/avr -o demo_13a.elf demo_13a.c
 */
#include <avr/io.h>
#include <avr/interrupt.h>
#include <stdint.h>
#include "avr_mcu_section.h"

AVR_MCU(1200000, "attiny13a");          /* 9.6 MHz RC oscillator with CKDIV8 */

volatile uint8_t ticks;
volatile uint16_t sample;

ISR(TIM0_OVF_vect)
{
    if (++ticks >= 44) {                /* 44 * 256 * 64 / 1.2 MHz = 0.94 s */
        ticks = 0;
        PORTB ^= _BV(PB0);
        ADCSRA |= _BV(ADSC);
    }
}

int main(void)
{
    DDRB = _BV(PB0) | _BV(PB2);
    PORTB = _BV(PB3);                   /* pull-up on SW0 */
    ADMUX = _BV(MUX1);                  /* ADC2 = PB4, VCC reference */
    ADCSRA = _BV(ADEN) | _BV(ADPS2) | _BV(ADPS1);
    TCCR0B = _BV(CS01) | _BV(CS00);     /* clk/64 */
    TIMSK0 = _BV(TOIE0);
    sei();
    for (;;) {
        if (PINB & _BV(PB3))
            PORTB &= ~_BV(PB2);
        else
            PORTB |= _BV(PB2);
        if (ADCSRA & _BV(ADIF)) {
            sample = ADC;
            ADCSRA |= _BV(ADIF);
        }
    }
}
