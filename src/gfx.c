/*
 * PicoWikiGuide - drawing on a 2 bit per pixel frame buffer: 4 pixels a
 * byte, MSB first, 0 = white .. 3 = black.  Rows are `stride` bytes.
 */
#include "gfx.h"

static inline void plot(uint8_t *row, int x, int v, int colour)
{
	uint8_t *d = row + (x >> 2);
	int sh = 6 - 2 * (x & 3);
	int old = (*d >> sh) & 3, nv;
	if (colour == BLACK)
		nv = v > old ? v : old;		/* ink adds */
	else if (colour == WHITE)
		nv = 3 - v < old ? 3 - v : old;	/* white ink on dark */
	else if (colour == SHADE)
		nv = old > 1 ? old : 1;		/* light grey under */
	else
		nv = v ? 3 - old : old;		/* invert */
	*d = (uint8_t)((*d & ~(3 << sh)) | (nv << sh));
}

void gfx_blit(uint8_t *buf, int stride, int rows, int x, int y,
	      const uint8_t *bits, int w, int h, int colour)
{
	int nb = (w + 7) >> 3, r, i, maxx = stride * 4;
	for (r = 0; r < h; ++r) {
		const uint8_t *src = bits + r * nb;
		int yy = y + r;
		uint8_t *row;
		if (yy < 0 || yy >= rows)
			continue;
		row = buf + yy * stride;
		for (i = 0; i < w; ++i) {
			int xx = x + i;
			if (xx < 0 || xx >= maxx || !(src[i >> 3] & (0x80 >> (i & 7))))
				continue;
			plot(row, xx, 3, colour);
		}
	}
}

void gfx_blit2(uint8_t *buf, int stride, int rows, int x, int y,
	       const uint8_t *bits, int w, int h, int colour)
{
	int nb = (w * 2 + 7) >> 3, r, i, maxx = stride * 4;
	for (r = 0; r < h; ++r) {
		const uint8_t *src = bits + r * nb;
		int yy = y + r;
		uint8_t *row;
		if (yy < 0 || yy >= rows)
			continue;
		row = buf + yy * stride;
		for (i = 0; i < w; ++i) {
			int xx = x + i, v = (src[i >> 2] >> (6 - 2 * (i & 3))) & 3;
			if (!v || xx < 0 || xx >= maxx)
				continue;
			plot(row, xx, v, colour);
		}
	}
}

void gfx_hline(uint8_t *buf, int stride, int rows, int x, int y, int w, int colour)
{
	int maxx = stride * 4;
	uint8_t *row;
	if (y < 0 || y >= rows)
		return;
	if (x < 0) {
		w += x;
		x = 0;
	}
	if (x + w > maxx)
		w = maxx - x;
	if (w <= 0)
		return;
	row = buf + y * stride;
	if (colour == BLACK || colour == WHITE) {
		uint8_t full = colour == BLACK ? 0xff : 0x00;
		/* ragged start, whole bytes, ragged end */
		while (w > 0 && (x & 3)) {
			plot(row, x, 3, colour);
			x++;
			w--;
		}
		while (w >= 4) {
			row[x >> 2] = full;
			x += 4;
			w -= 4;
		}
	}
	while (w > 0) {
		plot(row, x, 3, colour);
		x++;
		w--;
	}
}

void gfx_dotted(uint8_t *buf, int stride, int rows, int x, int y, int w)
{
	int i, maxx = stride * 4;
	if (y < 0 || y >= rows)
		return;
	for (i = 0; i < w; i += 2) {
		int xx = x + i;
		if (xx >= 0 && xx < maxx)
			plot(buf + y * stride, xx, 3, BLACK);
	}
}

void gfx_fill(uint8_t *buf, int stride, int rows, int x, int y, int w, int h, int colour)
{
	int r;
	for (r = 0; r < h; ++r)
		gfx_hline(buf, stride, rows, x, y + r, w, colour);
}

void gfx_rect(uint8_t *buf, int stride, int rows, int x, int y, int w, int h, int colour)
{
	int r;
	gfx_hline(buf, stride, rows, x, y, w, colour);
	gfx_hline(buf, stride, rows, x, y + h - 1, w, colour);
	for (r = 1; r < h - 1; ++r) {
		gfx_hline(buf, stride, rows, x, y + r, 1, colour);
		gfx_hline(buf, stride, rows, x + w - 1, y + r, 1, colour);
	}
}
