/*
 * PicoWikiGuide - colour pictures over the grey screen
 *
 * The screen is drawn in 2 bit grey into the frame buffer.  A colour
 * picture is not: it is a rectangle of RGB332 pixels (in the picture
 * buffer) listed here, and whoever sends the frame buffer to a display
 * (the LCD driver, the PC's window and screenshots) takes the pixels
 * inside it from the picture instead.  Nothing goes round the frame
 * buffer, so a changed screen can never leave a picture half painted
 * over: the rows a picture covers, now or at the last present, are sent
 * again whenever the list changes.
 */
#ifndef OVERLAY_H
#define OVERLAY_H

#include "zw.h"

#define OV_MAX 8

typedef struct overlay {
	int16_t x, y;
	uint16_t w, h;
	const uint8_t *px;	/* w * h RGB332 (red in the top 3 bits) */
	uint32_t gen;		/* changes when the pixels at px change */
} overlay_t;

void ov_clear(void);	/* at the start of every screen drawn */
void ov_add(int x, int y, int w, int h, const uint8_t *px, uint32_t gen);
int ov_count(void);

/* row y of the screen as RGB888 (3 * SCREEN_W bytes): the 2 bit frame
 * buffer row in the given grey levels (0 white ... 3 black), with the
 * pictures over it */
void ov_row_rgb(const uint8_t *fb_row, int y, const uint8_t level[4], uint8_t *rgb);

/* for a driver that sends only changed rows: is row y covered by a
 * picture now or at the last ov_presented(), with the list changed? */
int ov_row_dirty(int y);
void ov_presented(void);
/* before a present: compares the list with the last one presented */
int ov_begin(void);

#endif
