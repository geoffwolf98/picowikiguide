/*
 * PicoWikiGuide - the changed-rows present (src/present.c) with colour
 * pictures, against a simulated panel: after every present the panel
 * must show exactly the frame buffer with the pictures over it, through
 * picture pages, text pages, another picture in the same place, a page
 * drawn again unchanged, and pictures moving.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "present.h"
#include "overlay.h"
#include "gfx.h"

static uint8_t panel[SCREEN_H][SCREEN_W * 3];
static uint8_t fb[SCREEN_STRIDE * SCREEN_H], shown[sizeof(fb)];
static int valid, cur_y, rows_sent;
static const uint8_t level[4] = { 0xfc, 0x9c, 0x4c, 0x00 };

static void start(int y0, int y1) { (void)y1; cur_y = y0; }
static void row(const uint8_t *rgb) { memcpy(panel[cur_y++], rgb, SCREEN_W * 3); rows_sent++; }
static void end(void) {}

static int check(const char *what)
{
	uint8_t want[SCREEN_W * 3];
	int y;
	rows_sent = 0;
	present_rows(fb, shown, &valid, level, start, row, end);
	for (y = 0; y < SCREEN_H; ++y) {
		ov_row_rgb(fb + y * SCREEN_STRIDE, y, level, want);
		if (memcmp(want, panel[y], sizeof(want)) != 0) {
			printf("FAIL %s: row %d differs (%d rows sent)\n", what, y, rows_sent);
			return 1;
		}
	}
	printf("ok   %s (%d rows sent)\n", what, rows_sent);
	return 0;
}

static void text(int seed)
{
	int i;
	srand((unsigned)seed);
	memset(fb, 0, sizeof(fb));
	for (i = 0; i < 2000; ++i)
		fb[rand() % (int)sizeof(fb)] = (uint8_t)rand();
}

int main(void)
{
	static uint8_t picA[200 * 150], picB[200 * 150], picC[100 * 80];
	int bad = 0;
	memset(picA, 0xe0, sizeof(picA));	/* red */
	memset(picB, 0x1c, sizeof(picB));	/* green */
	memset(picC, 0x03, sizeof(picC));	/* blue */

	text(1);
	ov_clear();
	bad += check("first screen, text");
	text(2);
	ov_clear();
	memset(fb + 40 * SCREEN_STRIDE, 0, 150 * SCREEN_STRIDE);	/* blank under the picture */
	ov_add(60, 40, 200, 150, picA, 1);
	bad += check("a page with a colour picture");
	ov_clear();
	ov_add(60, 40, 200, 150, picA, 1);
	bad += check("the same page drawn again (nothing to send)");
	if (rows_sent)
		bad++, printf("FAIL: rows sent for an unchanged screen\n");
	ov_clear();
	ov_add(60, 40, 200, 150, picB, 2);
	bad += check("another picture in the same place (blank fb rows unchanged)");
	text(3);
	ov_clear();
	bad += check("a text page after it");
	memset(fb, 0, sizeof(fb));
	ov_clear();
	ov_add(10, 200, 100, 80, picC, 3);
	ov_add(150, 20, 200, 150, picB, 2);
	bad += check("two pictures");
	ov_clear();
	ov_add(150, 20, 200, 150, picB, 2);
	bad += check("one of them gone, fb unchanged");
	ov_clear();
	ov_add(110, 21, 200, 150, picB, 2);
	bad += check("a picture moved a little");
	fb[100 * SCREEN_STRIDE + 3] ^= 0xff;	/* a focus box beside it */
	ov_clear();
	ov_add(110, 21, 200, 150, picB, 2);
	bad += check("a change in the frame buffer beside a picture");
	return bad != 0;
}
