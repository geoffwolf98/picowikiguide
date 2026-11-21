/*
 * zimwiki - bitmap fonts
 */
#include "font.h"
#include "gfx.h"
#include "utf8.h"
#include "inflate.h"

static font_t fallback;		/* a ZWFT fallback font (uncompressed) */

/* a ZWF2 fallback font (compressed blocks; see tools/packwikifont.py) */
#define FB2_BLOCK_MAX 4096
static struct {
	const uint8_t *ranges, *offs, *data;
	uint32_t count, nranges, nblocks, per, bits_total;
	uint32_t fp_h0, fp_final;
} fb2;
static uint8_t fb2_blk[FB2_BLOCK_MAX];	/* the last block inflated */
static uint32_t fb2_blk_no = 0xffffffffu;
static uint8_t fb2_ring[8][32];		/* glyph bitmaps handed out */
static unsigned fb2_ring_pos;
/* the fallback font is loaded the first time a character needs it, so
 * start-up does not wait for it (most pages never do) */
static int (*fallback_loader)(void);
static int fallback_tried;

void font_set_fallback_loader(int (*fn)(void))
{
	fallback_loader = fn;
	fallback_tried = 0;
}

static int set_fb2(const uint8_t *data, uint32_t len)
{
	uint32_t n, nr, nb, per, hdr;
	if (len < 32)
		return ZW_EFORMAT;
	n = zw_rd32(data + 4);
	per = data[10];
	nr = data[11];
	nb = zw_rd32(data + 12);
	hdr = 32 + nr * 8 + (nb + 1) * 4;
	if (!n || n > 65536 || !per || per * (5 + 32) > FB2_BLOCK_MAX || nb != (n + per - 1) / per ||
	    hdr > len || hdr + zw_rd32(data + 32 + nr * 8 + nb * 4) > len)
		return ZW_EFORMAT;
	memset(&fallback, 0, sizeof(fallback));
	fb2.ranges = data + 32;
	fb2.offs = data + 32 + nr * 8;
	fb2.data = data + hdr;
	fb2.count = n;
	fb2.nranges = nr;
	fb2.nblocks = nb;
	fb2.per = per;
	fb2.fp_h0 = zw_rd32(data + 16);
	fb2.fp_final = zw_rd32(data + 20);
	fb2.bits_total = zw_rd32(data + 24);
	fb2_blk_no = 0xffffffffu;
	return ZW_OK;
}

/* inflate block b into fb2_blk; returns its glyph count, or 0 */
static uint32_t fb2_load(uint32_t b)
{
	uint32_t ng = fb2.count - b * fb2.per;
	if (ng > fb2.per)
		ng = fb2.per;
	if (b != fb2_blk_no) {
		uint32_t o = zw_rd32(fb2.offs + b * 4), e = zw_rd32(fb2.offs + b * 4 + 4);
		fb2_blk_no = 0xffffffffu;
		if (e < o || zw_inflate(fb2_blk, sizeof(fb2_blk), fb2.data + o, e - o) < (long)(ng * 5))
			return 0;
		fb2_blk_no = b;
	}
	return ng;
}

static int fb2_glyph(uint32_t cp, glyph_t *g)
{
	uint32_t lo = 0, hi = fb2.nranges, gi, b, k, ng, off, j, size;
	const uint8_t *m;
	if (cp > 0xffff)
		return 0;
	while (lo < hi) {	/* the last range starting at or before cp */
		uint32_t mid = (lo + hi) / 2;
		if (zw_rd16(fb2.ranges + mid * 8) <= cp)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (!lo)
		return 0;
	m = fb2.ranges + (lo - 1) * 8;
	if (cp - zw_rd16(m) >= zw_rd16(m + 2))
		return 0;
	gi = zw_rd32(m + 4) + (cp - zw_rd16(m));
	if (gi >= fb2.count)
		return 0;
	b = gi / fb2.per;
	k = gi % fb2.per;
	if (!(ng = fb2_load(b)))
		return 0;
	off = ng * 5;
	for (j = 0; j < k; ++j)
		off += (uint32_t)((fb2_blk[j * 5 + 3] + 7) / 8) * fb2_blk[j * 5 + 4];
	m = fb2_blk + k * 5;
	size = (uint32_t)((m[3] + 7) / 8) * m[4];
	if (size > sizeof(fb2_ring[0]) || off + size > sizeof(fb2_blk))
		return 0;
	g->adv = m[0];
	g->xoff = (int8_t)m[1];
	g->top = (int8_t)m[2];
	g->w = m[3];
	g->h = m[4];
	g->bpp = 1;
	memcpy(fb2_ring[fb2_ring_pos], fb2_blk + off, size);
	g->bits = fb2_ring[fb2_ring_pos];
	fb2_ring_pos = (fb2_ring_pos + 1) % 8;
	return 1;
}

int font_expand_fallback(void)
{
	uint8_t *d, *rec, *bits;
	uint32_t b, gi = 0, r = 0, rcount = 0, cp = 0, boff = 0;
	if (!fb2.count)
		return fallback.count ? ZW_OK : ZW_EFORMAT;
	d = zw_alloc(12 + fb2.count * GLYPH_REC + fb2.bits_total);
	if (!d)
		return ZW_ENOMEM;
	memcpy(d, "ZWFT", 4);
	zw_wr32(d + 4, fb2.count);
	d[8] = fallback.ascent;
	d[9] = fallback.descent;
	d[10] = d[11] = 0;
	rec = d + 12;
	bits = rec + fb2.count * GLYPH_REC;
	for (b = 0; b < fb2.nblocks; ++b) {
		uint32_t ng = fb2_load(b), k, off;
		if (!ng)
			goto bad;
		off = ng * 5;
		for (k = 0; k < ng; ++k, ++gi) {
			const uint8_t *m = fb2_blk + k * 5;
			uint32_t size = (uint32_t)((m[3] + 7) / 8) * m[4];
			while (!rcount) {	/* next code point range */
				if (r >= fb2.nranges || zw_rd32(fb2.ranges + r * 8 + 4) != gi)
					goto bad;
				cp = zw_rd16(fb2.ranges + r * 8);
				rcount = zw_rd16(fb2.ranges + r * 8 + 2);
				r++;
			}
			if (off + size > sizeof(fb2_blk) || boff + size > fb2.bits_total)
				goto bad;
			zw_wr16(rec, (uint16_t)cp);
			memcpy(rec + 2, m, 5);
			rec[7] = 0;
			zw_wr32(rec + 8, boff);
			memcpy(bits + boff, fb2_blk + off, size);
			rec += GLYPH_REC;
			boff += size;
			off += size;
			cp++;
			rcount--;
		}
	}
	if (boff != fb2.bits_total)
		goto bad;
	memset(&fb2, 0, sizeof(fb2));
	return font_set_fallback(d, 12 + gi * GLYPH_REC + boff);
bad:
	zw_free(d);
	return ZW_EFORMAT;
}

int font_set_fallback(const uint8_t *data, uint32_t len)
{
	uint32_t n;
	if (len >= 4 && memcmp(data, "ZWF2", 4) == 0) {
		int r = set_fb2(data, len);
		if (r == ZW_OK) {
			fallback.ascent = data[8];
			fallback.descent = data[9];
		}
		return r;
	}
	memset(&fb2, 0, sizeof(fb2));
	if (len < 12 || memcmp(data, "ZWFT", 4) != 0)
		return ZW_EFORMAT;
	n = zw_rd32(data + 4);
	if (n > 65536 || 12 + n * GLYPH_REC > len)
		return ZW_EFORMAT;
	fallback.glyphs = data + 12;
	fallback.bits = data + 12 + n * GLYPH_REC;
	fallback.count = (uint16_t)(n > 65535 ? 65535 : n);
	fallback.ascent = data[8];
	fallback.descent = data[9];
	return ZW_OK;
}

/* direct index for Latin-1, filled on first use (0 = not looked up,
 * 0xffff = absent, else index + 1) */
static uint16_t latin1[FONT_COUNT][256];

static const uint8_t *find_slow(const font_t *f, uint32_t cp);

static const uint8_t *find(const font_t *f, uint32_t cp)
{
	int fi = (int)(f - fonts);
	if (cp < 256 && fi >= 0 && fi < FONT_COUNT) {
		uint16_t v = latin1[fi][cp];
		if (!v) {
			const uint8_t *r = find_slow(f, cp);
			v = r ? (uint16_t)((r - f->glyphs) / GLYPH_REC + 1) : 0xffff;
			latin1[fi][cp] = v;
		}
		return v == 0xffff ? NULL : f->glyphs + (uint32_t)(v - 1) * GLYPH_REC;
	}
	return find_slow(f, cp);
}

static const uint8_t *find_slow(const font_t *f, uint32_t cp)
{
	uint32_t lo = 0, hi = f->count;
	if (cp > 0xffff || !f->count)
		return NULL;
	while (lo < hi) {
		uint32_t mid = (lo + hi) / 2;
		uint32_t c = zw_rd16(f->glyphs + mid * GLYPH_REC);
		if (c < cp)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo < f->count && zw_rd16(f->glyphs + lo * GLYPH_REC) == cp)
		return f->glyphs + lo * GLYPH_REC;
	return NULL;
}

static void fill(const font_t *f, const uint8_t *r, glyph_t *g)
{
	g->adv = r[2];
	g->xoff = (int8_t)r[3];
	g->top = (int8_t)r[4];
	g->w = r[5];
	g->h = r[6];
	g->bits = f->bits + zw_rd32(r + 8);
	g->bpp = f->bpp == 2 ? 2 : 1;
}

/* full line height bracket pieces for display matrices, drawn rather
 * than taken from a font so the rows join up: FONT_PIECE + 0..13 is
 * [ top, middle, bottom, ] top, middle, bottom, ( top, middle, bottom,
 * ) top, middle, bottom, |, || */
#define PIECE_MAX_H 32
static uint8_t piece_set[2][14][PIECE_MAX_H];	/* normal, large */
static int piece_hs[2];

int font_big(int f)
{
	switch (f) {
	case FONT_TEXT: return FONT_TEXTL;
	case FONT_BOLD: return FONT_BOLDL;
	case FONT_ITALIC: return FONT_ITALICL;
	case FONT_BOLDITALIC: return FONT_BOLDITALICL;
	case FONT_TITLE: return FONT_TITLEL;
	case FONT_HEADING: return FONT_HEADINGL;
	case FONT_SMALL: return FONT_SMALLL;
	}
	return f;
}

static int is_big(int f)
{
	return f >= FONT_TEXTL && f <= FONT_SMALLL;
}

static void make_pieces(int big)
{
	int base = big ? FONT_TEXTL : FONT_TEXT;
	int h = fonts[base].ascent + fonts[base].descent, k, y;
	uint8_t (*piece_bits)[PIECE_MAX_H] = piece_set[big];
	if (h > PIECE_MAX_H)
		h = PIECE_MAX_H;
	piece_hs[big] = h;
	memset(piece_bits, 0, sizeof(piece_set[big]));
	for (y = 0; y < h; ++y) {
		/* square brackets: bar plus a tick at the end */
		for (k = 0; k < 3; ++k) {
			int top = k == 0 && y < 1, bot = k == 2 && y >= h - 1;
			if (top || bot)
				continue;
			piece_bits[k][y] = 0x40;		/* x = 1 */
			piece_bits[3 + k][y] = 0x20;		/* x = 2 */
			if ((k == 0 && y == 1) || (k == 2 && y == h - 2)) {
				piece_bits[k][y] |= 0x70;	/* x = 1..3 */
				piece_bits[3 + k][y] |= 0xe0;	/* x = 0..2 */
			}
		}
		/* round brackets: curve in at the two ends */
		piece_bits[6][y] = y < 3 ? (y == 0 ? 0x10 : 0x20) : 0x40;
		piece_bits[7][y] = 0x40;
		piece_bits[8][y] = y >= h - 3 ? (y == h - 1 ? 0x10 : 0x20) : 0x40;
		piece_bits[9][y] = y < 3 ? (y == 0 ? 0x40 : 0x20) : 0x10;
		piece_bits[10][y] = 0x10;
		piece_bits[11][y] = y >= h - 3 ? (y == h - 1 ? 0x40 : 0x20) : 0x10;
		piece_bits[12][y] = 0x40;
		piece_bits[13][y] = 0xa0;
	}
}

void font_glyph(int fi, uint32_t cp, glyph_t *g)
{
	static const uint8_t box_bits[] = {
		0xf8, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0xf8
	};
	const uint8_t *r;

	g->bpp = 1;
	if (cp >= FONT_PIECE && cp < FONT_PIECE + 14) {
		int big = is_big(fi);
		if (!piece_hs[big])
			make_pieces(big);
		g->bits = piece_set[big][cp - FONT_PIECE];
		g->w = 4;
		g->h = (uint8_t)piece_hs[big];
		g->adv = 5;
		g->xoff = 0;
		g->top = (int8_t)fonts[big ? FONT_TEXTL : FONT_TEXT].ascent;
		return;
	}
	/* invisible marks (byte order mark, soft hyphen, zero width and
	 * direction marks): nothing drawn, no width */
	if (cp == 0xfeff || cp == 0x00ad || (cp >= 0x200b && cp <= 0x200f) ||
	    (cp >= 0x202a && cp <= 0x202e) || (cp >= 0x2060 && cp <= 0x2064)) {
		memset(g, 0, sizeof(*g));
		g->bits = box_bits;
		return;
	}
	if (cp == 0x200a) {	/* hair space: 1 px, used to line up columns */
		memset(g, 0, sizeof(*g));
		g->bits = box_bits;
		g->adv = 1;
		return;
	}

	if ((r = find(&fonts[fi], cp)) != NULL) {
		fill(&fonts[fi], r, g);
		return;
	}
	/* sub and superscripts: small symbols before full size ones */
	if ((fi == FONT_SMALL || fi == FONT_SMALLL) &&
	    (r = find(&fonts[FONT_MATHSMALL], cp)) != NULL) {
		fill(&fonts[FONT_MATHSMALL], r, g);
		return;
	}
	/* styled fonts fall back to the regular text font first (the
	 * large ones to the large regular font, then the normal one) */
	if (is_big(fi) && fi != FONT_TEXTL && (r = find(&fonts[FONT_TEXTL], cp)) != NULL) {
		fill(&fonts[FONT_TEXTL], r, g);
		return;
	}
	if (fi != FONT_TEXT && (r = find(&fonts[FONT_TEXT], cp)) != NULL) {
		fill(&fonts[FONT_TEXT], r, g);
		return;
	}
	if ((r = find(&fonts[FONT_EXTRA], cp)) != NULL) {
		fill(&fonts[FONT_EXTRA], r, g);
		return;
	}
	if ((r = find(&fonts[FONT_MATH], cp)) != NULL) {
		fill(&fonts[FONT_MATH], r, g);
		return;
	}
	if (!fallback.count && !fb2.count && fallback_loader && !fallback_tried && cp >= 0x370 &&
	    !(cp >= 0x2000 && cp <= 0x200f) && cp != 0x202f && cp != 0xfeff &&
	    !(cp >= 0xfe00 && cp <= 0xfe0f) && !(cp >= 0x202a && cp <= 0x202e) &&
	    !(cp >= 0x2060 && cp <= 0x206f) && cp != 0x061c) {
		fallback_tried = 1;
		fallback_loader();
	}
	if (fallback.count && (r = find(&fallback, cp)) != NULL) {
		fill(&fallback, r, g);
		return;
	}
	if (fb2.count && fb2_glyph(cp, g))
		return;
	if (cp == 0xa0 || cp == 0x2009 || cp == 0x202f || cp == 0x2002 || cp == 0x2003) {
		font_glyph(fi, ' ', g);
		return;
	}
	/* zero width: combining marks, joiners, soft hyphen, controls */
	if ((cp >= 0x300 && cp < 0x370) || cp == 0xad || (cp >= 0x200b && cp <= 0x200f) ||
	    cp == 0xfeff || cp < 0x20 || (cp >= 0xfe00 && cp <= 0xfe0f) ||
	    (cp >= 0x202a && cp <= 0x202e) || (cp >= 0x2060 && cp <= 0x206f) ||
	    cp == 0x061c || (cp >= 0x180b && cp <= 0x180f) || (cp >= 0xfff9 && cp <= 0xfffb) ||
	    (cp >= 0x7f && cp < 0xa0)) {
		memset(g, 0, sizeof(*g));
		return;
	}
	g->adv = 7;
	g->xoff = 1;
	g->top = 9;
	g->w = 5;
	g->h = 8;
	g->bits = box_bits;
}

int font_height(int f)
{
	return fonts[f].ascent + fonts[f].descent + 1;
}

int font_ascent(int f)
{
	return fonts[f].ascent;
}

/* ASCII advances per font, filled on first use */
static uint8_t ascii_adv[FONT_COUNT][128];
static uint8_t ascii_ready[FONT_COUNT];

/* advance of each ASCII character in font f, filled on first use */
const uint8_t *font_ascii_adv(int f)
{
	if (f < 0 || f >= FONT_COUNT)
		return NULL;
	if (!ascii_ready[f]) {
		uint8_t c;
		glyph_t g;
		for (c = 0; c < 128; ++c) {
			font_glyph(f, c, &g);
			ascii_adv[f][c] = g.adv < 0xff ? (uint8_t)g.adv : 0xfe;
		}
		ascii_ready[f] = 1;
	}
	return ascii_adv[f];
}

int font_width(int f, const char *s, size_t len)
{
	const uint8_t *p = (const uint8_t *)s, *end = p + len;
	const uint8_t *adv = font_ascii_adv(f);
	int w = 0;
	while (p < end) {
		glyph_t g;
		uint32_t cp;
		if (*p < 0x80 && adv) {
			w += adv[*p++];
			continue;
		}
		cp = *p < 0x80 ? *p++ : utf8_next(&p, end);
		font_glyph(f, cp, &g);
		w += g.adv;
	}
	return w;
}

int font_draw(uint8_t *buf, int stride, int rows, int x, int baseline,
	      int f, const char *s, size_t len, int colour)
{
	const uint8_t *p = (const uint8_t *)s, *end = p + len;
	while (p < end) {
		glyph_t g;
		uint32_t cp = *p < 0x80 ? *p++ : utf8_next(&p, end);
		font_glyph(f, cp, &g);
		if (g.h && g.bpp == 2)
			gfx_blit2(buf, stride, rows, x + g.xoff, baseline - g.top,
				  g.bits, g.w, g.h, colour);
		else if (g.h)
			gfx_blit(buf, stride, rows, x + g.xoff, baseline - g.top,
				 g.bits, g.w, g.h, colour);
		x += g.adv;
	}
	return x;
}

static uint32_t builtin_hash(void);

uint32_t font_fingerprint(void)
{
	uint32_t h = builtin_hash();
	/* a compressed fallback font carries its part, worked out when it
	 * was made (pagecat --font-fingerprint --patch) from the same bytes
	 * the uncompressed form hashes; it holds only for these built-in
	 * fonts */
	if (fb2.count && fb2.fp_final && fb2.fp_h0 == h)
		return fb2.fp_final;
	if (fb2.count && font_expand_fallback() != ZW_OK)
		return h;
	/* the Unicode fallback font too (it decides widths of other scripts) */
	if (fallback.count) {
		uint32_t i, nbits = 0;
		for (i = 0; i < (uint32_t)fallback.count * GLYPH_REC; ++i)
			h = (h ^ fallback.glyphs[i]) * 16777619u;
		for (i = 0; i < fallback.count; ++i) {
			const uint8_t *r = fallback.glyphs + i * GLYPH_REC;
			uint32_t end = zw_rd32(r + 8) + (uint32_t)((r[5] + 7) / 8) * r[6];
			if (end > nbits)
				nbits = end;
		}
		for (i = 0; i < nbits; ++i)
			h = (h ^ fallback.bits[i]) * 16777619u;
	}
	return h;
}

int font_fallback_info(uint32_t *h0, uint32_t *stored_h0, uint32_t *stored_final)
{
	*h0 = builtin_hash();
	*stored_h0 = fb2.fp_h0;
	*stored_final = fb2.fp_final;
	return fb2.count ? 2 : fallback.count ? 1 : 0;
}

/* FNV-1a over every built-in font's metrics, glyph records and bitmaps:
 * a wiki laid out with other fonts would draw wrongly */
static uint32_t builtin_hash(void)
{
	uint32_t h = 2166136261u;
	int f;
	for (f = 0; f < FONT_COUNT; ++f) {
		const font_t *ft = &fonts[f];
		uint32_t i, nbits = 0;
		uint8_t m[4] = { ft->ascent, ft->descent, (uint8_t)ft->count, ft->bpp };
		for (i = 0; i < 4; ++i)
			h = (h ^ m[i]) * 16777619u;
		for (i = 0; i < (uint32_t)ft->count * GLYPH_REC; ++i)
			h = (h ^ ft->glyphs[i]) * 16777619u;
		/* bitmaps: up to the end of the highest glyph's bits */
		for (i = 0; i < ft->count; ++i) {
			const uint8_t *r = ft->glyphs + i * GLYPH_REC;
			uint32_t end = zw_rd32(r + 8) +
				       (uint32_t)((r[5] * (ft->bpp == 2 ? 2 : 1) + 7) / 8) * r[6];
			if (end > nbits)
				nbits = end;
		}
		for (i = 0; i < nbits; ++i)
			h = (h ^ ft->bits[i]) * 16777619u;
	}
	return h;
}
