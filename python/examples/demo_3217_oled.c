/*
 * GUI demo for the ATtiny3217 with a 128x64 SSD1306 on TWI0 (boards/oled_3217.json).
 *   PB0 SCL, PB1 SDA   TWI0 default pins, SSD1306 at 0x3C
 *   PA0                LED0, toggles with every counter update
 *
 * Draws a title, a text line, a live counter and a sweeping bar, streaming each
 * 8-pixel page straight from a 5x7 font (no framebuffer).
 *
 *   avr-gcc -mmcu=attiny3217 -Os -g -I../../simavr/sim/avr -o demo_3217_oled.elf demo_3217_oled.c
 */
#include <avr/io.h>
#include <stdint.h>
#include "avr_mcu_section.h"

AVR_MCU(3333333, "attiny3217");

#define OLED_ADDR 0x3C

static void twi_wait(void)
{
    uint16_t budget = 20000;
    while (budget-- && !(TWI0.MSTATUS & (TWI_WIF_bm | TWI_RIF_bm)))
        ;
}

static void twi_start(void) { TWI0.MADDR = OLED_ADDR << 1; twi_wait(); }
static void twi_put(uint8_t b) { TWI0.MDATA = b; twi_wait(); }
static void twi_stop(void) { TWI0.MCTRLB = TWI_MCMD_STOP_gc; }

static void cmd(uint8_t c) { twi_start(); twi_put(0x00); twi_put(c); twi_stop(); }

static const uint8_t font[][5] = {              /* glyph columns, bit 0 = top row */
    {0x00, 0x00, 0x00, 0x00, 0x00},             /* ' ' */
    {0x3E, 0x51, 0x49, 0x45, 0x3E},             /* 0 */
    {0x00, 0x42, 0x7F, 0x40, 0x00},             /* 1 */
    {0x42, 0x61, 0x51, 0x49, 0x46},             /* 2 */
    {0x21, 0x41, 0x45, 0x4B, 0x31},             /* 3 */
    {0x18, 0x14, 0x12, 0x7F, 0x10},             /* 4 */
    {0x27, 0x45, 0x45, 0x45, 0x39},             /* 5 */
    {0x3C, 0x4A, 0x49, 0x49, 0x30},             /* 6 */
    {0x01, 0x71, 0x09, 0x05, 0x03},             /* 7 */
    {0x36, 0x49, 0x49, 0x49, 0x36},             /* 8 */
    {0x06, 0x49, 0x49, 0x29, 0x1E},             /* 9 */
    {0x00, 0x36, 0x36, 0x00, 0x00},             /* : */
    {0x7E, 0x11, 0x11, 0x11, 0x7E},             /* A */
    {0x7F, 0x41, 0x41, 0x22, 0x1C},             /* D */
    {0x7F, 0x49, 0x49, 0x49, 0x41},             /* E */
    {0x7F, 0x40, 0x40, 0x40, 0x40},             /* L */
    {0x3E, 0x41, 0x41, 0x41, 0x3E},             /* O */
    {0x01, 0x01, 0x7F, 0x01, 0x01},             /* T */
    {0x20, 0x54, 0x54, 0x54, 0x78},             /* a */
    {0x00, 0x44, 0x7D, 0x40, 0x00},             /* i */
    {0x7C, 0x04, 0x18, 0x04, 0x78},             /* m */
    {0x7C, 0x08, 0x04, 0x04, 0x78},             /* n */
    {0x7C, 0x08, 0x04, 0x04, 0x08},             /* r */
    {0x48, 0x54, 0x54, 0x54, 0x20},             /* s */
    {0x04, 0x3F, 0x44, 0x40, 0x20},             /* t */
    {0x1C, 0x20, 0x40, 0x20, 0x1C},             /* v */
    {0x0C, 0x50, 0x50, 0x50, 0x3C},             /* y */
    {0x38, 0x44, 0x44, 0x48, 0x7F},             /* d */
    {0x38, 0x44, 0x44, 0x44, 0x38},             /* o */
    {0x38, 0x54, 0x54, 0x54, 0x18},             /* e */
};

static uint8_t glyph(char c)
{
    static const char order[] = " 0123456789:ADELOTaimnrstvydoe";
    for (uint8_t i = 0; order[i]; i++)
        if (order[i] == c)
            return i;
    return 0;
}

/* Column `x` of a string drawn at pixel x0, `scale` times as wide (1 or 2). */
static uint8_t text_col(const char *s, uint8_t x0, uint8_t x, uint8_t scale)
{
    if (x < x0)
        return 0;
    uint8_t cell = (x - x0) / scale, ch = cell / 6, col = cell % 6;
    for (uint8_t i = 0; i < ch; i++)
        if (!s[i])
            return 0;
    return (s[ch] && col < 5) ? font[glyph(s[ch])][col] : 0;
}

static uint16_t ticks;

static uint8_t page_col(uint8_t page, uint8_t x, const char *count)
{
    switch (page) {
    case 0:  return text_col("ATtiny3217", 4, x, 2);
    case 2:  return text_col("simavr OLED", 4, x, 1);
    case 4:  return text_col(count, 4, x, 2);
    case 6:  return (x >= 4 && x < 4 + (ticks & 0x7F) && x < 124) ? 0x3C : (x == 4 || x == 123 ? 0x7E : 0);
    default: return 0;
    }
}

static void draw_page(uint8_t page, const char *count)
{
    cmd(0xB0 | page); cmd(0x00); cmd(0x10);
    twi_start();
    twi_put(0x40);
    for (uint8_t x = 0; x < 128; x++)
        twi_put(page_col(page, x, count));
    twi_stop();
}

int main(void)
{
    PORTA.DIRSET = PIN0_bm;
    TWI0.MBAUD = 10;
    TWI0.MCTRLA = TWI_ENABLE_bm;
    TWI0.MSTATUS = TWI_BUSSTATE_IDLE_gc;

    static const uint8_t init[] = {0xAE, 0xA8, 0x3F, 0xD3, 0x00, 0x40, 0xA0, 0xC0, 0xDA, 0x12,
                                   0x81, 0x7F, 0xA4, 0xA6, 0x20, 0x02, 0xD5, 0x80, 0x8D, 0x14, 0xAF};
    for (uint8_t i = 0; i < sizeof init; i++)
        cmd(init[i]);
    for (uint8_t p = 0; p < 8; p++)
        draw_page(p, "");

    char count[7] = "0000";
    for (;;) {
        uint16_t n = ticks;
        for (int8_t i = 3; i >= 0; i--) { count[i] = '0' + n % 10; n /= 10; }
        draw_page(4, count);
        draw_page(6, count);
        PORTA.OUTTGL = PIN0_bm;
        ticks++;
    }
}
