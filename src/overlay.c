/*
 * PicoWikiGuide - colour pictures over the grey screen
 */
#include "overlay.h"
#include "gfx.h"

static overlay_t cur[OV_MAX], last[OV_MAX];
static int ncur, nlast, changed = 1;

void ov_clear(void)
{
	ncur = 0;
}

void ov_add(int x, int y, int w, int h, const uint8_t *px, uint32_t gen)
{
	overlay_t *o;
	if (ncur >= OV_MAX || w <= 0 || h <= 0)
		return;
	o = &cur[ncur++];
	o->x = (int16_t)x;
	o->y = (int16_t)y;
	o->w = (uint16_t)w;
	o->h = (uint16_t)h;
	o->px = px;
	o->gen = gen;
}

int ov_count(void)
{
	return ncur;
}

static const uint8_t lv3[8] = { 0, 36, 73, 109, 146, 182, 219, 255 };
static const uint8_t lv2[4] = { 0, 85, 170, 255 };

void ov_row_rgb(const uint8_t *fb_row, int y, const uint8_t level[4], uint8_t *rgb)
{
	int x, i;
	for (x = 0; x < SCREEN_W; ++x) {
		uint8_t v = level[(fb_row[x >> 2] >> (6 - 2 * (x & 3))) & 3];
		rgb[x * 3] = rgb[x * 3 + 1] = rgb[x * 3 + 2] = v;
	}
	for (i = 0; i < ncur; ++i) {
		const overlay_t *o = &cur[i];
		const uint8_t *p;
		int x0 = o->x, x1 = o->x + o->w;
		if (y < o->y || y >= o->y + o->h)
			continue;
		p = o->px + (size_t)(y - o->y) * o->w;
		if (x0 < 0) {
			p -= x0;
			x0 = 0;
		}
		if (x1 > SCREEN_W)
			x1 = SCREEN_W;
		for (x = x0; x < x1; ++x, ++p) {
			rgb[x * 3] = lv3[*p >> 5];
			rgb[x * 3 + 1] = lv3[(*p >> 2) & 7];
			rgb[x * 3 + 2] = lv2[*p & 3];
		}
	}
}

static int same(void)
{
	int i;
	if (ncur != nlast)
		return 0;
	for (i = 0; i < ncur; ++i)
		if (cur[i].x != last[i].x || cur[i].y != last[i].y || cur[i].w != last[i].w ||
		    cur[i].h != last[i].h || cur[i].px != last[i].px || cur[i].gen != last[i].gen)
			return 0;
	return 1;
}

int ov_row_dirty(int y)
{
	int i;
	if (!changed)
		return 0;
	for (i = 0; i < ncur; ++i)
		if (y >= cur[i].y && y < cur[i].y + cur[i].h)
			return 1;
	for (i = 0; i < nlast; ++i)
		if (y >= last[i].y && y < last[i].y + last[i].h)
			return 1;
	return 0;
}

void ov_presented(void)
{
	memcpy(last, cur, sizeof(cur));
	nlast = ncur;
	changed = 0;
}

/* called by ov_row_dirty's users before a present: has the list changed? */
int ov_begin(void)
{
	changed = !same();
	return changed;
}
