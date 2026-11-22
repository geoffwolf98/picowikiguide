/*
 * PicoWikiGuide - sending only the changed rows of the screen
 *
 * The frame buffer (2 bit grey) with the colour pictures over it
 * (overlay.h) goes to a display that keeps what it was sent: only rows
 * that changed since the last time are sent, as RGB888.  A row changed
 * if its frame buffer bytes did, or if a colour picture covers it now or
 * did last time and the pictures changed.  The LCD driver sends rows over
 * SPI with this; the tests send them to a simulated panel.
 */
#ifndef PRESENT_H
#define PRESENT_H

#include "zw.h"

/* shown: the frame buffer as last sent (SCREEN_STRIDE * SCREEN_H);
 * *valid: 0 before anything was sent (everything goes).  send(y0, y1)
 * starts a run of rows, row(rgb) sends each of them in turn. */
void present_rows(const uint8_t *fb, uint8_t *shown, int *valid, const uint8_t level[4],
		  void (*start)(int y0, int y1), void (*row)(const uint8_t *rgb), void (*end)(void));

#endif
