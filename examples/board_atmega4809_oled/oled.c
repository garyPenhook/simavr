/*
	oled.c

	OpenGL host "board" for the modern-AVR (atmega4809) SSD1306 OLED demo. It
	loads the I2C or SPI firmware, wires the reused ssd1306_virt part to the
	modern TWI0 / SPI0 (named '0' — unlike classic cores), runs the AVR in a
	real-time-throttled thread, and renders the 128x64 panel with ssd1306_glut.

	Usage:
	    ./oled [i2c|spi]          open a window driven by that bus (default i2c)
	    ./oled [i2c|spi] check    headless: run briefly, report lit pixels, exit
	                              (for machines/CI with no display)

	This is the visual counterpart that proves the modern TWI0 and SPI0 models
	carry a real display protocol end to end.

	Copyright 2026 simavr authors

 	This file is part of simavr.

	simavr is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	simavr is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with simavr.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#if __APPLE__
#include <GLUT/glut.h>
#else
#include <GL/glut.h>
#endif

#include "sim_avr.h"
#include "avr_ioport.h"
#include "avr_spi.h"
#include "avr_twi.h"
#include "sim_elf.h"

#include "ssd1306_virt.h"
#include "ssd1306_glut.h"

avr_t * avr = NULL;
ssd1306_t ssd1306;

static int win_width, win_height;
static const float pix_size = 4.0f;

/*
 * Wire the SSD1306 part to the *modern* SPI0 / TWI0 via the shared ssd1306_virt
 * connect helpers. Modern (AVRxt) cores register their SPI/TWI bus under the
 * char name '0' (classic cores use integer name 0), so we pass '0' through.
 */
static void
connect_spi(void)
{
	// control GPIOs match atmega4809_oled_spi.c: CS=PC0, DC=PC1, RST=PC2
	ssd1306_wiring_t wiring = {
		.chip_select	  = { .port = 'C', .pin = 0 },
		.data_instruction = { .port = 'C', .pin = 1 },
		.reset		  = { .port = 'C', .pin = 2 },
	};
	ssd1306_connect(&ssd1306, &wiring, '0');
}

static void
connect_i2c(void)
{
	// the I2C firmware drives no reset line; PC2 is left undriven (stays low,
	// so the falling-edge reset hook never fires).
	ssd1306_wiring_t wiring = {
		.reset = { .port = 'C', .pin = 2 },
	};
	ssd1306_connect_twi(&ssd1306, &wiring, '0');
}

static unsigned
count_lit_pixels(void)
{
	unsigned n = 0;
	for (int p = 0; p < 8; p++)
		for (int c = 0; c < 128; c++)
			n += __builtin_popcount(ssd1306.vram[p][c]);
	return n;
}

/* Run the AVR throttled to real time so the animation is watchable. */
static void *
avr_run_thread(void * param)
{
	struct timespec start;
	clock_gettime(CLOCK_MONOTONIC, &start);
	for (;;) {
		avr_run(avr);
		uint64_t sim_ns = (uint64_t)((avr->cycle * 1000000000.0) / avr->frequency);
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		uint64_t wall_ns = (uint64_t)(now.tv_sec - start.tv_sec) * 1000000000ull
				 + (now.tv_nsec - start.tv_nsec);
		if (sim_ns > wall_ns + 1000000ull) {
			struct timespec s = { 0, (long)(sim_ns - wall_ns) };
			while (s.tv_nsec >= 1000000000L) { s.tv_nsec -= 1000000000L; s.tv_sec++; }
			nanosleep(&s, NULL);
		}
	}
	return NULL;
}

static void
displayCB(void)
{
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0, win_width, 0, win_height, 0, 10);
	glScalef(1, -1, 1);
	glTranslatef(0, -win_height, 0);
	glMatrixMode(GL_MODELVIEW);
	glPushMatrix();
	glLoadIdentity();
	ssd1306_gl_draw(&ssd1306);
	glPopMatrix();
	glutSwapBuffers();
}

static void
timerCB(int i)
{
	glutTimerFunc(1000 / 30, timerCB, 0);
	glutPostRedisplay();
}

static void
keyCB(unsigned char key, int x, int y)
{
	if (key == 'q' || key == 0x1b)
		exit(0);
}

int
main(int argc, char *argv[])
{
	const char *bus = (argc > 1) ? argv[1] : "i2c";
	int headless = (argc > 2) && strcmp(argv[2], "check") == 0;

	int is_spi = strcmp(bus, "spi") == 0;
	if (!is_spi && strcmp(bus, "i2c") != 0) {
		fprintf(stderr, "usage: %s [i2c|spi] [check]\n", argv[0]);
		return 2;
	}

	char fname[64];
	snprintf(fname, sizeof(fname), "atmega4809_oled_%s.axf", bus);

	elf_firmware_t f = {{0}};
	if (elf_read_firmware(fname, &f) < 0) {
		fprintf(stderr, "%s: unable to load firmware %s\n", argv[0], fname);
		return 1;
	}
	printf("firmware %s f=%d mmcu=%s  bus=%s\n",
		fname, (int)f.frequency, f.mmcu, bus);

	avr = avr_make_mcu_by_name(f.mmcu);
	if (!avr) {
		fprintf(stderr, "%s: AVR '%s' not known\n", argv[0], f.mmcu);
		return 1;
	}
	avr_init(avr);
	avr_load_firmware(avr, &f);

	ssd1306_init(avr, &ssd1306, 128, 64);
	if (is_spi)
		connect_spi();
	else
		connect_i2c();

	if (headless) {
		// run ~300 ms and confirm the panel framebuffer got drawn
		uint64_t stop = avr->cycle + avr->frequency * 3 / 10;
		int st = cpu_Running;
		while (st != cpu_Done && st != cpu_Crashed && avr->cycle < stop)
			st = avr_run(avr);
		unsigned lit = count_lit_pixels();
		printf("%s  %s: %u/%u pixels lit after 300 ms\n",
			lit > 0 ? "PASS" : "FAIL", bus, lit, 8 * 128 * 8);
		return lit > 0 ? 0 : 1;
	}

	printf("\nSSD1306 128x64 over %s. A checkerboard with a moving bar should\n"
	       "appear. Press 'q' (or Esc) in the window to quit.\n\n",
	       is_spi ? "SPI0" : "TWI0 (I2C)");

	pthread_t t;
	pthread_create(&t, NULL, avr_run_thread, NULL);

	glutInit(&argc, argv);
	win_width = 128 * pix_size;
	win_height = 64 * pix_size;
	glutInitDisplayMode(GLUT_RGB | GLUT_DOUBLE);
	glutInitWindowSize(win_width, win_height);
	glutCreateWindow(is_spi ? "simavr - atmega4809 SSD1306 (SPI)"
				: "simavr - atmega4809 SSD1306 (I2C)");
	glutDisplayFunc(displayCB);
	glutKeyboardFunc(keyCB);
	glutTimerFunc(1000 / 30, timerCB, 0);
	ssd1306_gl_init(pix_size, SSD1306_GL_WHITE);

	glutMainLoop();
	return 0;
}
