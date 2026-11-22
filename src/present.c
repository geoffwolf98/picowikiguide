/*
 * PicoWikiGuide - sending only the changed rows of the screen
 */
#include "present.h"
#include "overlay.h"
#include "gfx.h"

void present_rows(const uint8_t *fb, uint8_t *shown, int *valid, const uint8_t level[4],
		  void (*start)(int y0, int y1), void (*row)(const uint8_t *rgb), void (*end)(void))
{
	static uint8_t line[SCREEN_W * 3];
	int y0 = 0, y1, y;
	ov_begin();		/* colour pictures changed: their rows go again */
#define CHANGED(yy) (!*valid || ov_row_dirty(yy) || \
		     memcmp(fb + (yy) * SCREEN_STRIDE, shown + (yy) * SCREEN_STRIDE, SCREEN_STRIDE) != 0)
	for (;;) {
		while (y0 < SCREEN_H && !CHANGED(y0))
			y0++;
		if (y0 >= SCREEN_H)
			break;
		y1 = y0;
		while (y1 + 1 < SCREEN_H && CHANGED(y1 + 1))
			y1++;
		start(y0, y1);
		for (y = y0; y <= y1; ++y) {
			ov_row_rgb(fb + y * SCREEN_STRIDE, y, level, line);
			row(line);
		}
		end();
		memcpy(shown + y0 * SCREEN_STRIDE, fb + y0 * SCREEN_STRIDE,
		       (size_t)(y1 - y0 + 1) * SCREEN_STRIDE);
		y0 = y1 + 1;
	}
#undef CHANGED
	*valid = 1;
	ov_presented();
}
