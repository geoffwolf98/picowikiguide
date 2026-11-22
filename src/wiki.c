/*
 * PicoWikiGuide - reading a prepared wiki and drawing its pages
 */
#include "wiki.h"
#include "font.h"
#include "gfx.h"
#define ZSTD_STATIC_LINKING_ONLY
#include "zstd.h"

#define MARGIN_L 4
#define MARGIN_R 5

/* The big buffers are fixed (in .bss, not the heap): one wiki is open at
 * a time, and opening and closing wikis then cannot split the heap into
 * pieces too small for them.  Sizes: a dictionary up to DICT_MAX (zimprep
 * makes 112 KiB), the zstd context and dictionary tables (checked against
 * zstd's own estimates when a wiki opens). */
#define DICT_MAX  (112 * 1024)
#define CBUF_MAX  ZSTD_COMPRESSBOUND(FRAME_MAX)
#define DCTX_WS   (96 * 1024)
#define DDICT_WS  (28 * 1024)
static uint8_t s_frame[FRAME_MAX];
static uint8_t s_cbuf[CBUF_MAX];
#ifdef PWG_DICT_IN_FLASH
/* the PicoCalc keeps the dictionary in flash (pico/dictflash.c): it is
 * only read once loaded, and that saves 112 KB of RAM */
#include "dictflash.h"
static void (*s_notice)(void);
#else
static uint8_t s_dict[DICT_MAX];
#endif
static uint64_t s_dctx[DCTX_WS / 8];
static uint64_t s_ddict[DDICT_WS / 8];
static wiki_t *s_open;		/* the wiki using them */

void wiki_set_notice(void (*fn)(void))
{
#ifdef PWG_DICT_IN_FLASH
	s_notice = fn;
#else
	(void)fn;
#endif
}

uint8_t *wiki_scratch(uint32_t *len)
{
	/* the compressed frame buffer only holds data inside wiki_page() */
	*len = sizeof(s_cbuf);
	return s_cbuf;
}

static int rd(const zw_io_t *io, uint64_t off, void *buf, uint32_t len)
{
	return zw_io_read(io, off, buf, len) < 0 ? ZW_EIO : 0;
}

static void copy_str(char *dst, const uint8_t *src, int n)
{
	memcpy(dst, src, (size_t)n);
	dst[n - 1] = 0;
}

int wiki_open(wiki_t *w, const zw_io_t *toc, const zw_io_t *data)
{
	uint8_t *h;
	int r = ZW_EFORMAT;

	if (s_open)
		return ZW_ENOMEM;	/* one at a time */
	memset(w, 0, sizeof(*w));
	w->toc = *toc;
	w->data = *data;
	w->frame_no = TOC_NONE;
	h = s_frame;		/* the header goes through the frame buffer */
	if (rd(toc, 0, h, TOC_HDR_SIZE) < 0)
		return ZW_EIO;
	if (zw_rd32(h + TOC_H_MAGIC) != TOC_MAGIC || zw_rd16(h + TOC_H_VERSION) != TOC_VERSION)
		goto out;
	r = ZW_EUNSUPPORTED;
	if ((zw_rd32(h + TOC_H_MUST) & ~TOC_MUST_KNOWN) || h[TOC_H_COMP] != TOC_COMP_ZSTD_DICT ||
	    h[TOC_H_TEXTPX] != TEXT_PX || zw_rd16(h + TOC_H_PAGE_W) != PAGE_W)
		goto out;
	w->fonts = zw_rd32(h + TOC_H_FONTS);
	if (w->fonts != font_fingerprint())
		goto out;
	r = ZW_EFORMAT;
	w->articles = zw_rd32(h + TOC_H_ARTICLES);
	w->frames = zw_rd32(h + TOC_H_FRAMES);
	w->pieces = zw_rd32(h + TOC_H_PIECES);
	w->main = zw_rd32(h + TOC_H_MAIN);
	w->art_off = zw_rd32(h + TOC_H_ART_OFF);
	w->frame_off = zw_rd32(h + TOC_H_FRAME_OFF);
	w->sec_off = zw_rd32(h + TOC_H_SEC_OFF);
	w->sec_len = zw_rd64(h + TOC_H_SEC_LEN);
	w->data_len = zw_rd64(h + TOC_H_DATA_LEN);
	w->dict_len = zw_rd32(h + TOC_H_DICT_LEN);
	copy_str(w->title, h + TOC_H_TITLE, TOC_H_TITLE_LEN);
	copy_str(w->name, h + TOC_H_NAME, TOC_H_NAME_LEN);
	copy_str(w->lang, h + TOC_H_LANG, TOC_H_LANG_LEN);
	copy_str(w->date, h + TOC_H_DATE, TOC_H_DATE_LEN);
	if (!w->articles || h[TOC_H_PPF] != 1)
		goto out;

	r = ZW_EUNSUPPORTED;
	if (w->dict_len > DICT_MAX)
		goto out;
	r = ZW_ENOMEM;
	if (ZSTD_estimateDCtxSize() > sizeof(s_dctx) ||
	    ZSTD_estimateDDictSize(w->dict_len, ZSTD_dlm_byRef) > sizeof(s_ddict))
		goto out;
	w->frame = s_frame;
	w->cbuf = s_cbuf;
	w->ccap = sizeof(s_cbuf);
	w->dctx = ZSTD_initStaticDCtx(s_dctx, sizeof(s_dctx));
	if (!w->dctx)
		goto out;
	if (w->dict_len) {
		r = ZW_EIO;
#ifdef PWG_DICT_IN_FLASH
		/* through the compressed frame buffer (free while opening) */
		w->dict = (uint8_t *)dict_flash_place(toc, zw_rd32(h + TOC_H_DICT_OFF), w->dict_len,
						      s_cbuf, sizeof(s_cbuf), s_notice);
		if (!w->dict)
			goto out;
#else
		w->dict = s_dict;
		if (rd(toc, zw_rd32(h + TOC_H_DICT_OFF), w->dict, w->dict_len) < 0)
			goto out;
#endif
		r = ZW_ENOMEM;
		/* by reference: the dictionary is not copied again */
		w->ddict = (void *)ZSTD_initStaticDDict(s_ddict, sizeof(s_ddict), w->dict, w->dict_len,
							ZSTD_dlm_byRef, ZSTD_dct_auto);
		if (!w->ddict)
			goto out;
	}
	s_open = w;
	return 0;
out:
	memset(w, 0, sizeof(*w));
	w->frame_no = TOC_NONE;
	return r;
}

void wiki_close(wiki_t *w)
{
	/* the buffers are fixed: nothing to free */
	if (s_open == w)
		s_open = NULL;
	memset(w, 0, sizeof(*w));
	w->frame_no = TOC_NONE;
}

int wiki_article(wiki_t *w, uint32_t art, wiki_art_t *a)
{
	uint8_t r[TOC_ART_REC];
	if (art >= w->articles)
		return ZW_ERANGE;
	if (rd(&w->toc, w->art_off + (uint64_t)art * TOC_ART_REC, r, TOC_ART_REC) < 0)
		return ZW_EIO;
	a->first_frame = zw_rd32(r);
	a->pages = zw_rd16(r + 4);
	a->sections = zw_rd16(r + 6);
	a->sec_off = zw_rd32(r + 8);
	if (a->pages && (a->first_frame >= w->frames || a->pages > w->frames - a->first_frame))
		return ZW_EFORMAT;
	return 0;
}

static int load_frame(wiki_t *w, uint32_t f)
{
	uint8_t r[TOC_FRAME_REC];
	uint64_t off;
	uint32_t clen;
	size_t n;
	if (f == w->frame_no)
		return 0;
	if (f >= w->frames)
		return ZW_ERANGE;
	if (rd(&w->toc, w->frame_off + (uint64_t)f * TOC_FRAME_REC, r, TOC_FRAME_REC) < 0)
		return ZW_EIO;
	off = zw_rd32(r) | ((uint64_t)r[4] << 32);
	clen = r[5] | ((uint32_t)r[6] << 8) | ((uint32_t)r[7] << 16);
	if (!clen || off + clen > w->data_len)
		return ZW_EFORMAT;
	if (clen > w->ccap)
		return ZW_EFORMAT;	/* more than any frame compresses to */
	if (rd(&w->data, off, w->cbuf, clen) < 0)
		return ZW_EIO;
	w->frame_no = TOC_NONE;
	if (w->ddict)
		n = ZSTD_decompress_usingDDict(w->dctx, w->frame, FRAME_MAX, w->cbuf, clen, w->ddict);
	else
		n = ZSTD_decompressDCtx(w->dctx, w->frame, FRAME_MAX, w->cbuf, clen);
	if (ZSTD_isError(n))
		return ZW_EFORMAT;
	zw_stats.bytes_decompressed += (uint32_t)n;
	w->frame_no = f;
	w->frame_len = (uint32_t)n;
	return 0;
}

static int varint(const uint8_t **p, const uint8_t *end, uint32_t *v)
{
	uint32_t x = 0;
	int sh = 0;
	while (*p < end && sh < 35) {
		uint8_t c = *(*p)++;
		x |= (uint32_t)(c & 0x7f) << sh;
		if (!(c & 0x80)) {
			*v = x;
			return 0;
		}
		sh += 7;
	}
	return -1;
}

static int parse_page(const uint8_t *p, const uint8_t *end, page_t *pg)
{
	unsigned nl, i, k;
	pg->nlines = pg->nitems = pg->nlinks = 0;
	if (p >= end)
		return ZW_EFORMAT;
	nl = *p++;
	for (i = 0; i < nl; ++i) {
		page_line_t *ln = &pg->lines[pg->nlines < PAGE_MAX_LINES ? pg->nlines : PAGE_MAX_LINES - 1];
		unsigned ni;
		if (end - p < 6)
			return ZW_EFORMAT;
		ln->y = (int16_t)zw_rd16(p);
		ln->h = p[2];
		ln->base = p[3];
		ln->deco = p[4];
		p += 5;
		ln->deco_x = 0;
		ln->number = 0;
		if ((ln->deco & 0x7f) == PD_BULLET || (ln->deco & 0x7f) == PD_NUMBER) {
			if (end - p < 4)
				return ZW_EFORMAT;
			ln->deco_x = (int16_t)zw_rd16(p);
			ln->number = zw_rd16(p + 2);
			p += 4;
		}
		if (p >= end)
			return ZW_EFORMAT;
		ni = *p++;
		ln->first = pg->nitems;
		ln->nitems = 0;
		for (k = 0; k < ni; ++k) {
			uint8_t type;
			uint32_t len;
			if (p >= end)
				return ZW_EFORMAT;
			type = *p++;
			if (type >= PI_EXT) {	/* a later kind of item: skip it */
				if (varint(&p, end, &len) < 0 || len > (uint32_t)(end - p))
					return ZW_EFORMAT;
				p += len;
				continue;
			}
			if (end - p < 4)
				return ZW_EFORMAT;
			if (pg->nitems < PAGE_MAX_ITEMS && type < FONT_COUNT) {
				page_item_t *it = &pg->items[pg->nitems];
				it->font = type;
				it->x = (int16_t)zw_rd16(p);
				it->dy = (int8_t)p[2];
				it->slot = p[3];
				it->line = (uint8_t)pg->nlines;
				p += 4;
				if (varint(&p, end, &len) < 0 || len > (uint32_t)(end - p))
					return ZW_EFORMAT;
				it->text = (const char *)p;
				it->len = (uint16_t)len;
				p += len;
				pg->nitems++;
				ln->nitems++;
			} else {
				p += 4;
				if (varint(&p, end, &len) < 0 || len > (uint32_t)(end - p))
					return ZW_EFORMAT;
				p += len;
			}
		}
		if (pg->nlines < PAGE_MAX_LINES)
			pg->nlines++;
	}
	if (p >= end)
		return ZW_EFORMAT;
	pg->nlinks = *p++;
	for (i = 0; i < pg->nlinks; ++i)
		if (varint(&p, end, &pg->links[i]) < 0)
			return ZW_EFORMAT;
	/* items whose slot points past the table are plain text */
	for (i = 0; i < pg->nitems; ++i)
		if (pg->items[i].slot != PI_NOLINK && pg->items[i].slot >= pg->nlinks)
			pg->items[i].slot = PI_NOLINK;
	return 0;
}

int wiki_page(wiki_t *w, const wiki_art_t *a, uint32_t page, page_t *pg)
{
	const uint8_t *p, *end;
	uint32_t f, n;
	int r;
	if (page >= a->pages)
		return ZW_ERANGE;
	f = a->first_frame + page;	/* one page per frame (checked at open) */
	if ((r = load_frame(w, f)) < 0)
		return r;
	p = w->frame;
	end = w->frame + w->frame_len;
	if (end - p < 3)
		return ZW_EFORMAT;
	n = *p++;
	if (!n || end - p < 2 * (int)n)
		return ZW_EFORMAT;
	{
		const uint8_t *body = p + 2 * n;
		uint32_t len = zw_rd16(p);	/* page 0 of the frame */
		if (len > (uint32_t)(end - body))
			return ZW_EFORMAT;
		return parse_page(body, body + len, pg);
	}
}

int wiki_sections(wiki_t *w, const wiki_art_t *a, wiki_section_t *out, uint32_t max)
{
	uint64_t off;
	uint32_t i, n = a->sections < max ? a->sections : max;
	uint8_t h[4];
	if (!a->sections || a->sec_off == TOC_NONE)
		return 0;
	off = (uint64_t)w->sec_off + a->sec_off;
	for (i = 0; i < n; ++i) {
		if (off + 4 > (uint64_t)w->sec_off + w->sec_len || rd(&w->toc, off, h, 4) < 0)
			return i;
		{
			uint32_t n = h[3] < sizeof(out[i].title) - 1 ? h[3] : sizeof(out[i].title) - 1;
			out[i].page = zw_rd16(h);
			out[i].level = h[2];
			if (rd(&w->toc, off + 4, out[i].title, n) < 0)
				return i;
			if (n < h[3]) {	/* cut: drop a partial last character */
				while (n && ((uint8_t)out[i].title[n - 1] & 0xc0) == 0x80)
					n--;
				if (n && (uint8_t)out[i].title[n - 1] >= 0xc0)
					n--;
			}
			out[i].title[n] = 0;
		}
		off += 4 + h[3];
	}
	return (int)n;
}

/* width of an item's text without trailing spaces */
static int item_w(const page_item_t *it)
{
	size_t n = it->len;
	while (n && it->text[n - 1] == ' ')
		n--;
	return font_width(it->font, it->text, n);
}

void page_item_rect(const page_t *pg, uint32_t k, int *x, int *y, int *wd, int *h)
{
	const page_item_t *it = &pg->items[k];
	const page_line_t *ln = &pg->lines[it->line];
	*x = it->x;
	*y = ln->y;
	*wd = item_w(it);
	*h = ln->h;
}

static void draw_number(uint8_t *fb, int x_right, int base, int n)
{
	char s[8], t[8];
	int k = 0, i;
	if (n <= 0)
		return;
	if (n & 0x4000) {
		n &= 0x3fff;
		while (n && k < 6) {
			n--;
			t[k++] = (char)('a' + n % 26);
			n /= 26;
		}
	} else {
		while (n && k < 6) {
			t[k++] = (char)('0' + n % 10);
			n /= 10;
		}
	}
	for (i = 0; i < k; ++i)
		s[i] = t[k - 1 - i];
	s[k++] = '.';
	font_draw(fb, SCREEN_STRIDE, SCREEN_H, x_right - font_width(FONT_TEXTL, s, (size_t)k),
		  base, FONT_TEXTL, s, (size_t)k, BLACK);
}

void page_draw(const page_t *pg, uint8_t *fb, int y0, int focus)
{
	unsigned i, k;
	int fa = -1, fb_ = -1;
	/* the focused link: the run of items around `focus` with its slot */
	if (focus >= 0 && focus < pg->nitems && pg->items[focus].slot != PI_NOLINK) {
		uint8_t s = pg->items[focus].slot;
		fa = fb_ = focus;
		while (fa > 0 && pg->items[fa - 1].slot == s)
			fa--;
		while (fb_ + 1 < pg->nitems && pg->items[fb_ + 1].slot == s)
			fb_++;
	}
	for (i = 0; i < pg->nlines; ++i) {
		const page_line_t *ln = &pg->lines[i];
		int y = y0 + ln->y, base = y + ln->base;
		for (k = ln->first; k < (unsigned)ln->first + ln->nitems; ++k) {
			const page_item_t *it = &pg->items[k];
			int w;
			font_draw(fb, SCREEN_STRIDE, SCREEN_H, it->x, base + it->dy, it->font,
				  it->text, it->len, BLACK);
			if (it->slot == PI_NOLINK)
				continue;
			w = item_w(it);
			if ((int)k >= fa && (int)k <= fb_) {
				/* the focused link: a box round it and a solid underline; the
				 * text itself is untouched, so it stays sharp */
				gfx_rect(fb, SCREEN_STRIDE, SCREEN_H, it->x - 3, y, w + 6, ln->h, BLACK);
				gfx_rect(fb, SCREEN_STRIDE, SCREEN_H, it->x - 4, y - 1, w + 8, ln->h + 2, BLACK);
				gfx_hline(fb, SCREEN_STRIDE, SCREEN_H, it->x, base + it->dy + 2, w, BLACK);
			} else {
				gfx_dotted(fb, SCREEN_STRIDE, SCREEN_H, it->x, base + it->dy + 2, w);
			}
		}
		switch (ln->deco & 0x7f) {
		case PD_BULLET:
			gfx_fill(fb, SCREEN_STRIDE, SCREEN_H, ln->deco_x + 2, base - 7, 4, 4, BLACK);
			break;
		case PD_NUMBER:
			draw_number(fb, ln->deco_x + 15, base, ln->number);
			break;
		case PD_RULE:
			gfx_hline(fb, SCREEN_STRIDE, SCREEN_H, MARGIN_L + 20, y + 2,
				  SCREEN_W - MARGIN_L - MARGIN_R - 40, BLACK);
			break;
		case PD_HEAD_RULE:
			gfx_hline(fb, SCREEN_STRIDE, SCREEN_H, MARGIN_L, y + ln->h - 2,
				  SCREEN_W - MARGIN_L - MARGIN_R, BLACK);
			break;
		}
		if (ln->deco & PD_QUOTE)
			gfx_fill(fb, SCREEN_STRIDE, SCREEN_H, MARGIN_L + 2, y, 2, ln->h, BLACK);
	}
}
