/*
 * PicoWikiGuide - reading a prepared wiki and drawing its pages
 */
#include "wiki.h"
#include "overlay.h"
#include "tjpgd.h"
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
/* the pictures of the page last drawn, decoded: grey ones 2 bits a pixel
 * (drawn into the frame buffer), colour ones RGB332 (shown over it, see
 * overlay.h).  Kept, so redrawing the page reads nothing. */
#ifndef PICBUF_SIZE
#define PICBUF_SIZE (311u * 255u)
#endif
static uint8_t s_picbuf[PICBUF_SIZE] __attribute__((aligned(8)));
typedef struct pcache {
	uint32_t number, off;
	uint16_t w, h;
	uint16_t row0, row1;	/* rows kept (all of them unless space ran out) */
	uint8_t colour;
} pcache_t;
#define PC_MAX (2 * PAGE_MAX_PICS)	/* two pages in a scrolled view */
static pcache_t s_pc[PC_MAX];
static unsigned s_npc;
static uint32_t s_pc_used, s_pc_gen;
static void pc_reset(void)
{
	s_npc = 0;
	s_pc_used = 0;
	s_pc_gen++;
}

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
	w->pic_count = zw_rd32(h + TOC_H_PICS);
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

int wiki_set_pictures(wiki_t *w, const zw_io_t *io)
{
	uint8_t h[PIC_FILE_HDR];
	if (zw_io_read(io, 0, h, sizeof(h)) < 0)
		return ZW_EIO;
	if (memcmp(h, "PWGI", 4) != 0 || zw_rd32(h + 4) != 1 || zw_rd32(h + 8) != w->pic_count)
		return ZW_EFORMAT;	/* not this wiki's pictures */
	w->pics = *io;
	w->have_pics = 1;
	pc_reset();
	return ZW_OK;
}

/* JPEG (colour records) into the picture buffer as RGB332, with a 4 x 4
 * ordered dither: TJpgDec hands over blocks of RGB888 */
static const uint8_t bayer4[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };
static uint8_t s_jdwork[TJPGD_WORKSPACE_SIZE] __attribute__((aligned(8)));

typedef struct jctx {
	const uint8_t *data;
	size_t len, pos;
	uint8_t *out;		/* w * (row1 - row0) RGB332 */
	unsigned w, h, row0, row1;
} jctx_t;

static size_t jd_in(JDEC *jd, uint8_t *buf, size_t n)
{
	jctx_t *c = jd->device;
	if (n > c->len - c->pos)
		n = c->len - c->pos;
	if (buf)
		memcpy(buf, c->data + c->pos, n);
	c->pos += n;
	return n;
}

static int jd_out(JDEC *jd, void *bitmap, JRECT *r)
{
	jctx_t *c = jd->device;
	const uint8_t *p = bitmap;
	unsigned x, y;
	for (y = r->top; y <= r->bottom; ++y)
		for (x = r->left; x <= r->right; ++x, p += 3) {
			/* threshold 0..15 -> a nudge of up to one step of each channel */
			int t = bayer4[(y & 3) * 4 + (x & 3)];
			int rr = p[0] + ((t * 2 - 15) * 255) / (7 * 32);
			int gg = p[1] + ((t * 2 - 15) * 255) / (7 * 32);
			int bb = p[2] + ((t * 2 - 15) * 255) / (3 * 32);
			rr = rr < 0 ? 0 : rr > 255 ? 255 : rr;
			gg = gg < 0 ? 0 : gg > 255 ? 255 : gg;
			bb = bb < 0 ? 0 : bb > 255 ? 255 : bb;
			if (x < c->w && y >= c->row0 && y < c->row1)
				c->out[(y - c->row0) * c->w + x] = (uint8_t)(((rr * 7 + 127) / 255) << 5 |
								 ((gg * 7 + 127) / 255) << 2 |
								 ((bb * 3 + 127) / 255));
		}
	return 1;
}

/* picture n (rows row0 to row1, or all when row1 is 0; only colour ones
 * can be cut) into the picture buffer after what is there; its entry */
static pcache_t *load_picture(wiki_t *w, uint32_t n, unsigned row0, unsigned row1)
{
	uint8_t e[8];
	uint64_t off;
	uint32_t len, size, dw, dh;
	unsigned i;
	pcache_t *pc;
	for (i = 0; i < s_npc; ++i)
		if (s_pc[i].number == n && (row1 ? s_pc[i].row0 <= row0 && s_pc[i].row1 >= row1 :
					    s_pc[i].row0 == 0 && s_pc[i].row1 == s_pc[i].h))
			return &s_pc[i];
	if (!w->have_pics || n >= w->pic_count || s_npc >= PC_MAX)
		return NULL;
	if (zw_io_read(&w->pics, PIC_FILE_HDR + (uint64_t)n * 8, e, 8) < 0)
		return NULL;
	off = zw_rd32(e) | (uint64_t)e[4] << 32;
	len = e[5] | (uint32_t)e[6] << 8 | (uint32_t)e[7] << 16;
	if (len < 8)
		return NULL;	/* zimprep could not convert it */
	/* the record: u16 w, u16 h, u8 format, 3 x 0, data (through the
	 * compressed frame buffer, free outside wiki_page) */
	if (zw_io_read(&w->pics, off, s_cbuf, 8) < 0)
		return NULL;
	dw = zw_rd16(s_cbuf);
	dh = zw_rd16(s_cbuf + 2);
	if (!dw || !dh || dw > 311 || dh > 255)
		return NULL;
	pc = &s_pc[s_npc];
	pc->number = n;
	pc->w = (uint16_t)dw;
	pc->h = (uint16_t)dh;
	pc->colour = s_cbuf[4] == 2;
	pc->off = s_pc_used;
	if (!row1 || !pc->colour) {
		row0 = 0;
		row1 = dh;
	}
	if (row0 >= row1 || row1 > dh)
		return NULL;
	pc->row0 = (uint16_t)row0;
	pc->row1 = (uint16_t)row1;
	size = pc->colour ? dw * (row1 - row0) : (uint32_t)((dw * 2 + 7) / 8) * dh;
	if (size > PICBUF_SIZE - s_pc_used)
		return NULL;	/* no room left (the next page starts afresh) */
	if (s_cbuf[4] == 0) {
		if (len - 8 != size || zw_io_read(&w->pics, off + 8, s_picbuf + pc->off, size) < 0)
			return NULL;
	} else if (s_cbuf[4] == 1) {
		size_t r;
		if (len > sizeof(s_cbuf) || zw_io_read(&w->pics, off, s_cbuf, len) < 0)
			return NULL;
		r = ZSTD_decompressDCtx(w->dctx, s_picbuf + pc->off, size, s_cbuf + 8, len - 8);
		if (ZSTD_isError(r) || r != size)
			return NULL;
	} else if (s_cbuf[4] == 2) {
		JDEC jd;
		jctx_t c;
		if (len > sizeof(s_cbuf) || zw_io_read(&w->pics, off, s_cbuf, len) < 0)
			return NULL;
		memset(&jd, 0, sizeof(jd));
		c.data = s_cbuf + 8;
		c.len = len - 8;
		c.pos = 0;
		c.out = s_picbuf + pc->off;
		c.w = dw;
		c.h = dh;
		c.row0 = row0;
		c.row1 = row1;
		if (jd_prepare(&jd, jd_in, s_jdwork, sizeof(s_jdwork), &c) != JDR_OK ||
		    jd.width != dw || jd.height != dh || jd_decomp(&jd, jd_out, 0) != JDR_OK)
			return NULL;
	} else {
		return NULL;	/* a later kind */
	}
	s_pc_used += size;
	s_npc++;
	return pc;
}

/* the visible rows of a view page's picture */
static int vis_rows(const view_page_t *v, const page_pic_t *pp, unsigned *r0, unsigned *r1)
{
	int top = v->y0 + pp->y, a = v->clip0 - top, b = v->clip1 - top;
	if (a < 0)
		a = 0;
	if (b > pp->h)
		b = pp->h;
	if (b <= a)
		return 0;
	*r0 = (unsigned)a;
	*r1 = (unsigned)b;
	return 1;
}

static const pcache_t *find_pc(const page_pic_t *pp, unsigned r0, unsigned r1)
{
	unsigned k;
	for (k = 0; k < s_npc; ++k)
		if (s_pc[k].number == pp->number && s_pc[k].w == pp->w && s_pc[k].h == pp->h &&
		    s_pc[k].row0 <= r0 && s_pc[k].row1 >= r1)
			return &s_pc[k];
	return NULL;
}

/* all the view's visible pictures in the buffer: kept if they are there
 * already, else the buffer starts afresh with whole pictures if they all
 * fit, or else (big ones meeting in a scrolled view) only the visible
 * rows of the colour ones, the most showing first, so a sliver of one
 * is what goes without when not all fit */
#define VIS_MAX 24
static void load_view_pictures(wiki_t *w, const view_page_t *v, int nv)
{
	static struct vis { const page_pic_t *pp; unsigned r0, r1; uint32_t area; } vis[VIS_MAX];
	struct vis t;
	int i, n = 0, all = 1, crop, j;
	unsigned k, r0, r1;
	uint32_t whole = 0;
	for (i = 0; i < nv; ++i)
		for (k = 0; k < v[i].npics; ++k) {
			const page_pic_t *pp = &v[i].pics[k];
			if (!vis_rows(&v[i], pp, &r0, &r1))
				continue;
			whole += (uint32_t)pp->w * pp->h;
			if (!find_pc(pp, r0, r1))
				all = 0;
			if (n < VIS_MAX) {
				vis[n].pp = pp;
				vis[n].r0 = r0;
				vis[n].r1 = r1;
				vis[n].area = (uint32_t)pp->w * (r1 - r0);
				for (j = n++; j > 0 && vis[j].area > vis[j - 1].area; --j) {
					t = vis[j];
					vis[j] = vis[j - 1];
					vis[j - 1] = t;
				}
			}
		}
	if (all)
		return;
	pc_reset();
	crop = whole > PICBUF_SIZE;
	for (i = 0; i < n; ++i)
		if (!find_pc(vis[i].pp, vis[i].r0, vis[i].r1) &&
		    !(crop ? NULL : load_picture(w, vis[i].pp->number, 0, 0)))
			load_picture(w, vis[i].pp->number, vis[i].r0, vis[i].r1);
}

void wiki_draw_scratch(uint8_t **frame, page_t **pg)
{
	uint32_t at = (s_pc_used + 7u) & ~7u;
	uint32_t need = FRAME_MAX + ((sizeof(page_t) + 7u) & ~7u);
	if (at + need > PICBUF_SIZE) {
		pc_reset();
		at = 0;
	}
	*pg = (page_t *)(void *)(s_picbuf + at);
	*frame = s_picbuf + at + ((sizeof(page_t) + 7u) & ~7u);
}

int wiki_picture_ok(wiki_t *w, const page_pic_t *pp)
{
	unsigned k;
	(void)w;
	for (k = 0; k < s_npc; ++k)
		if (s_pc[k].number == pp->number && s_pc[k].w == pp->w && s_pc[k].h == pp->h)
			return 1;
	return 0;
}

void wiki_draw_view_pictures(wiki_t *w, const view_page_t *v, int nv, uint8_t *fb)
{
	int i;
	unsigned k, r0, r1;
	load_view_pictures(w, v, nv);
	for (i = 0; i < nv; ++i)
		for (k = 0; k < v[i].npics; ++k) {
			const page_pic_t *pp = &v[i].pics[k];
			const pcache_t *c;
			int top = v[i].y0 + pp->y;
			if (!vis_rows(&v[i], pp, &r0, &r1))
				continue;
			c = find_pc(pp, r0, r1);
			if (!c) {
				gfx_rect(fb, SCREEN_STRIDE, SCREEN_H, pp->x, top + (int)r0, pp->w,
					 (int)(r1 - r0), BLACK);
			} else if (c->colour) {
				ov_add(pp->x, top + (int)r0, pp->w, (int)(r1 - r0),
				       s_picbuf + c->off + (r0 - c->row0) * pp->w, s_pc_gen);
			} else {
				unsigned stride = (unsigned)((pp->w * 2 + 7) / 8);
				gfx_blit2(fb, SCREEN_STRIDE, SCREEN_H, pp->x, top + (int)r0,
					  s_picbuf + c->off + r0 * stride, pp->w, (int)(r1 - r0), BLACK);
			}
		}
}

void wiki_draw_pictures(wiki_t *w, const page_t *pg, uint8_t *fb, int y0)
{
	view_page_t v;
	v.pics = pg->pics;
	v.npics = pg->npics;
	v.y0 = y0;
	v.clip0 = 0;
	v.clip1 = SCREEN_H;
	wiki_draw_view_pictures(w, &v, 1, fb);
}

void wiki_close(wiki_t *w)
{
	/* the buffers are fixed: nothing to free */
	if (s_open == w)
		s_open = NULL;
	pc_reset();		/* another wiki's numbers */
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

/* frame f decompressed into dst (FRAME_MAX bytes) */
static int read_frame(wiki_t *w, uint32_t f, uint8_t *dst, uint32_t *len)
{
	uint8_t r[TOC_FRAME_REC];
	uint64_t off;
	uint32_t clen;
	size_t n;
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
	if (w->ddict)
		n = ZSTD_decompress_usingDDict(w->dctx, dst, FRAME_MAX, w->cbuf, clen, w->ddict);
	else
		n = ZSTD_decompressDCtx(w->dctx, dst, FRAME_MAX, w->cbuf, clen);
	if (ZSTD_isError(n))
		return ZW_EFORMAT;
	zw_stats.bytes_decompressed += (uint32_t)n;
	*len = (uint32_t)n;
	return 0;
}

static int load_frame(wiki_t *w, uint32_t f)
{
	int r;
	if (f == w->frame_no)
		return 0;
	w->frame_no = TOC_NONE;
	if ((r = read_frame(w, f, w->frame, &w->frame_len)) < 0)
		return r;
	w->frame_no = f;
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
	pg->nlines = pg->nitems = pg->nlinks = pg->npics = 0;
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
				if (type == PI_PIC && len >= 10 && pg->npics < PAGE_MAX_PICS) {
					page_pic_t *pp = &pg->pics[pg->npics++];
					pp->x = (int16_t)zw_rd16(p);
					pp->y = ln->y;
					pp->w = zw_rd16(p + 2);
					pp->h = zw_rd16(p + 4);
					pp->number = zw_rd32(p + 6);
				}
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

static int page_from_frame(const uint8_t *frame, uint32_t frame_len, page_t *pg);

int wiki_page(wiki_t *w, const wiki_art_t *a, uint32_t page, page_t *pg)
{
	int r;
	if (page >= a->pages)
		return ZW_ERANGE;
	/* one page per frame (checked at open) */
	if ((r = load_frame(w, a->first_frame + page)) < 0)
		return r;
	return page_from_frame(w->frame, w->frame_len, pg);
}

int wiki_page_into(wiki_t *w, const wiki_art_t *a, uint32_t page, page_t *pg, uint8_t *buf)
{
	uint32_t len;
	int r;
	if (page >= a->pages)
		return ZW_ERANGE;
	if ((r = read_frame(w, a->first_frame + page, buf, &len)) < 0)
		return r;
	return page_from_frame(buf, len, pg);
}

int page_content_h(const page_t *pg)
{
	int i, h = 0;
	for (i = 0; i < (int)pg->nlines; ++i) {
		const page_line_t *ln = &pg->lines[i];
		if (ln->y == PAGE_H - 17 && i == (int)pg->nlines - 1 && i > 0)
			break;	/* the "picture on the next page" note */
		if (ln->y + ln->h > h)
			h = ln->y + ln->h;
	}
	return h;
}

static int page_from_frame(const uint8_t *frame, uint32_t frame_len, page_t *pg)
{
	const uint8_t *p = frame, *end = frame + frame_len;
	uint32_t n;
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
