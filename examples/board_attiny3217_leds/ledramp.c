/*
	ledramp.c

	OpenGL host "board" for the modern-AVR (attiny3217) LED demo. It opens a
	window that draws PORTA as eight LEDs and animates them live from the running
	firmware: the eight IOPORT pin IRQs of PORTA drive the on/off state, and the
	AVR runs in a background thread, real-time throttled so the animation is
	watchable. USART0 output is echoed to the terminal.

	This is the visual counterpart to the headless board_attiny3217 / the
	board_modern_avr coverage runner: same modern engine, but with a GUI. It is
	the tinyAVR-1 sibling of examples/board_atmega4809_leds.

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
#include <time.h>
#include <pthread.h>
#if __APPLE__
#include <GLUT/glut.h>
#else
#include <GL/glut.h>
#endif

#include "sim_avr.h"
#include "avr_ioport.h"
#include "avr_uart.h"
#include "sim_elf.h"

static const char * fname = "attiny3217_ledramp.axf";

avr_t * avr = NULL;
static volatile uint8_t porta_state = 0;	// current PORTA pin levels (8 LEDs)
static float pixsize = 64;
static int window;

/* PORTA pin change -> update our LED bitmap. */
static void
porta_changed_hook(struct avr_irq_t * irq, uint32_t value, void * param)
{
	if (value)
		porta_state |= (1 << irq->irq);
	else
		porta_state &= ~(1 << irq->irq);
}

/* USART0 byte -> echo to the terminal. */
static void
usart0_out_hook(struct avr_irq_t * irq, uint32_t value, void * param)
{
	putchar((char)value);
	fflush(stdout);
}

static void
displayCB(void)
{
	glClear(GL_COLOR_BUFFER_BIT);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();

	float grid = pixsize;
	float size = grid * 0.8f;
	glBegin(GL_QUADS);
	for (int i = 0; i < 8; i++) {
		char on = (porta_state & (1 << i)) != 0;
		// lit = bright red, off = dark red, so all 8 LEDs are always visible
		if (on)
			glColor3f(1.f, 0.1f, 0.1f);
		else
			glColor3f(0.15f, 0.f, 0.f);
		float x = i * grid;
		float y = 0;
		glVertex2f(x + size, y + size);
		glVertex2f(x, y + size);
		glVertex2f(x, y);
		glVertex2f(x + size, y);
	}
	glEnd();
	glutSwapBuffers();
}

static void
timerCB(int i)
{
	static uint8_t old = 0xff;
	glutTimerFunc(1000 / 60, timerCB, 0);
	if (old != porta_state) {
		old = porta_state;
		glutPostRedisplay();
	}
}

static void
keyCB(unsigned char key, int x, int y)
{
	if (key == 'q' || key == 0x1b /* esc */)
		exit(0);
}

/* Run the AVR, but throttled to real time so the LEDs are watchable. */
static void *
avr_run_thread(void * param)
{
	struct timespec start;
	clock_gettime(CLOCK_MONOTONIC, &start);

	for (;;) {
		avr_run(avr);

		// how far the simulated clock has advanced, in nanoseconds
		uint64_t sim_ns = (uint64_t)((avr->cycle * 1000000000.0) / avr->frequency);
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		uint64_t wall_ns = (uint64_t)(now.tv_sec - start.tv_sec) * 1000000000ull
				 + (now.tv_nsec - start.tv_nsec);
		// if the sim has run ahead of the wall clock, sleep off the difference
		if (sim_ns > wall_ns + 1000000ull) {
			struct timespec s = { 0, (long)(sim_ns - wall_ns) };
			while (s.tv_nsec >= 1000000000L) { s.tv_nsec -= 1000000000L; s.tv_sec++; }
			nanosleep(&s, NULL);
		}
	}
	return NULL;
}

int
main(int argc, char *argv[])
{
	elf_firmware_t f = {{0}};

	if (elf_read_firmware(fname, &f) < 0) {
		fprintf(stderr, "%s: unable to load firmware %s\n", argv[0], fname);
		exit(1);
	}
	printf("firmware %s f=%d mmcu=%s\n", fname, (int)f.frequency, f.mmcu);

	avr = avr_make_mcu_by_name(f.mmcu);
	if (!avr) {
		fprintf(stderr, "%s: AVR '%s' not known\n", argv[0], f.mmcu);
		exit(1);
	}
	avr_init(avr);
	avr_load_firmware(avr, &f);

	// the 8 PORTA pins drive the 8 LEDs (modern PORT reuses the IOPORT IRQ mesh)
	for (int i = 0; i < 8; i++)
		avr_irq_register_notify(
			avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ('A'), i),
			porta_changed_hook, NULL);

	// echo USART0 to the terminal (modern USART reuses the UART IRQ mesh)
	avr_irq_register_notify(
		avr_io_getirq(avr, AVR_IOCTL_UART_GETIRQ('0'), UART_IRQ_OUTPUT),
		usart0_out_hook, NULL);

	printf("\nWindow shows PORTA as 8 LEDs; a lit LED bounces across them.\n"
	       "Press 'q' (or Esc) in the window to quit.\n\n");

	// run the AVR in its own thread, throttled to real time
	pthread_t t;
	pthread_create(&t, NULL, avr_run_thread, NULL);

	glutInit(&argc, argv);
	glutInitDisplayMode(GLUT_RGB | GLUT_DOUBLE);
	glutInitWindowSize(8 * pixsize, 1 * pixsize);
	window = glutCreateWindow("simavr - attiny3217 PORTA LEDs");

	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0, 8 * pixsize, 0, 1 * pixsize, 0, 10);
	glScalef(1, -1, 1);
	glTranslatef(0, -1 * pixsize, 0);

	glutDisplayFunc(displayCB);
	glutKeyboardFunc(keyCB);
	glutTimerFunc(1000 / 60, timerCB, 0);

	glutMainLoop();
	return 0;
}
