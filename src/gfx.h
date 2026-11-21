/*
 * PicoWikiGuide - drawing on a 2 bit per pixel frame buffer (4 greys):
 * rows of `stride` bytes, 4 pixels a byte, MSB first, 0 = white,
 * 3 = black.
 */
#ifndef GFX_H
#define GFX_H

#include "zw.h"

#define SCREEN_W      320
#define SCREEN_H      320
#define SCREEN_STRIDE 80	/* bytes per row: 2 bits a pixel */

/* SHADE: a light grey background (pixels become at least the lightest grey) */
enum { WHITE = 0, BLACK = 1, INVERT = 2, SHADE = 3 };

/* blit a 1 bit bitmap (rows of ceil(w/8) bytes) */
void gfx_blit(uint8_t *buf, int stride, int rows, int x, int y,
	      const uint8_t *bits, int w, int h, int colour);
/* blit a 2 bit bitmap (rows of ceil(w/4) bytes, 0 = no ink .. 3 = full);
 * BLACK: ink darkens, WHITE: light ink on dark, INVERT: flips inked pixels */
void gfx_blit2(uint8_t *buf, int stride, int rows, int x, int y,
	       const uint8_t *bits, int w, int h, int colour);
void gfx_fill(uint8_t *buf, int stride, int rows, int x, int y, int w, int h, int colour);
void gfx_hline(uint8_t *buf, int stride, int rows, int x, int y, int w, int colour);
void gfx_dotted(uint8_t *buf, int stride, int rows, int x, int y, int w);
void gfx_rect(uint8_t *buf, int stride, int rows, int x, int y, int w, int h, int colour);

#endif
