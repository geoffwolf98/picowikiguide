/*
 * zimprep - prepare a Kiwix ZIM wiki for PicoWikiGuide
 *
 *   zimprep [options] INPUT.zim OUTDIR
 *
 *   -n NAME        folder and file name on the card, 1-8 letters/digits
 *                  (default: from the input file name)
 *   --threads N    worker threads (default: all CPUs)
 *   --level N      zstd level (default 19)
 *   --dict-kb N    zstd dictionary size in KiB (default and most 112, 0 = none)
 *   --font FILE    the Unicode fallback font (default: fonts/WIKIFONT.BIN
 *                  next to the build folder); must match the firmware's
 *   --pictures none|lead|all   pictures (default all; none: text only)
 *   --grey         grey pictures (default colour; --colour still accepted)
 *   --jpeg-quality N, --pic-height N   colour quality, most picture height
 *   --version
 *
 * The version and the picture settings are printed at the start of every
 * run.  A ZIM without pictures (nopic, mini) makes a text-only wiki.
 *
 * Writes OUTDIR/NAME/ with NAME.TOC, NAME.P00..., NAME.IDX, NAME.HIS
 * and NAME.FAV.  Copy the NAME folder to the top of the SD card.
 *
 * Every article is converted to text (Gen2's HTML converter, footnotes
 * and citations left out), laid out for the 320 x 320 screen with the
 * 18 px fonts and cut into pages.  Each page is one zstd frame with the
 * wiki's dictionary.  Pictures, video, audio, styles, scripts and the
 * full-text index are never copied.  See DESIGN.md for the format.
 *
 * INPUT may also be the base of a split set: "wiki.zim" for wiki.zimaa,
 * wiki.zimab, ...
 */
#define _FILE_OFFSET_BITS 64
#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define ZSTD_STATIC_LINKING_ONLY
#include "zstd.h"
#include "zdict.h"

#include "zw.h"
#include "card.h"
#include "fold.h"
#include "index.h"
#include "font.h"
#include "zim.h"
#include "doc.h"
#include "layout.h"
#include "pictures.h"
#include "hostio.h"

#define PIECE_SHIFT  31
#define BATCH        4096	/* articles converted between writes */
#define GRAB         16		/* articles a worker takes at a time */

static void die(const char *fmt, const char *arg)
{
	fprintf(stderr, "zimprep: ");
	fprintf(stderr, fmt, arg);
	fprintf(stderr, "\n");
	exit(1);
}

static void *xmalloc(size_t n)
{
	void *p = malloc(n ? n : 1);
	if (!p)
		die("out of memory", "");
	return p;
}

static void *xrealloc(void *p, size_t n)
{
	p = realloc(p, n ? n : 1);
	if (!p)
		die("out of memory", "");
	return p;
}

/* ------------------------------------------------------------------ */
/* growable byte buffer */

typedef struct buf {
	uint8_t *p;
	size_t n, cap;
} buf_t;

static void b_need(buf_t *b, size_t more)
{
	if (b->n + more > b->cap) {
		b->cap = (b->n + more) * 2 + 256;
		b->p = xrealloc(b->p, b->cap);
	}
}
static void b_u8(buf_t *b, unsigned v) { b_need(b, 1); b->p[b->n++] = (uint8_t)v; }
static void b_u16(buf_t *b, unsigned v) { b_need(b, 2); zw_wr16(b->p + b->n, (uint16_t)v); b->n += 2; }

static void b_mem(buf_t *b, const void *s, size_t n) { b_need(b, n); memcpy(b->p + b->n, s, n); b->n += n; }
static void b_varint(buf_t *b, uint32_t v)
{
	do {
		uint8_t c = v & 0x7f;
		v >>= 7;
		b_u8(b, v ? c | 0x80 : c);
	} while (v);
}

/* ------------------------------------------------------------------ */
/* the wiki being read */

static const char *g_input;
static zim_t g_zim;		/* main thread's reader */
static uint32_t *g_arts;	/* article number -> entry */
static uint32_t g_narts;
static uint32_t *g_entry2art;	/* entry -> article number or TOC_NONE */

static int is_html(const zim_t *z, uint16_t mime)
{
	return mime < z->nmime && strncmp(z->mime[mime], "text/html", 9) == 0;
}

/* ------------------------------------------------------------------ */
/* one article -> pages */

typedef struct worker {
	host_files_t files;
	zim_t z;
	ZSTD_CCtx *cctx;
	pthread_t th;
} worker_t;

typedef struct art_out {
	buf_t frames;		/* compressed frames, back to back */
	uint32_t *fsize;	/* each frame's length */
	uint32_t nframes;
	buf_t sec;		/* the article's section records */
	uint16_t nsec;
	uint32_t raw;		/* bytes before compression */
	int failed;
} art_out_t;

static ZSTD_CDict *g_cdict;
static int g_level = 19;

/* resolve every link of the document to an article number */
static uint32_t *resolve_links(zim_t *z, const doc_t *d)
{
	uint32_t *map = xmalloc((d->nlinks + 1) * sizeof(uint32_t));
	uint32_t k;
	for (k = 0; k < d->nlinks; ++k) {
		char ns;
		const char *u = doc_link(d, k, &ns);
		uint32_t t, fin;
		zim_dirent_t de;
		map[k] = TOC_NONE;
		if (!u || !*u)
			continue;
		if (zim_find(z, ns, u, &t) == 0 && zim_follow(z, t, &fin, &de) == 0 &&
		    de.ns == z->content_ns && is_html(z, de.mime) && fin < z->entry_count)
			map[k] = g_entry2art[fin];
	}
	return map;
}

static int heading_level(int kind)
{
	return kind == BK_H2 ? 2 : kind == BK_H3 ? 3 : kind == BK_H4 ? 4 : 0;
}

static unsigned deco_code(const lay_line_t *ln)
{
	unsigned d = PD_NONE;
	switch (ln->deco) {
	case LD_BULLET: d = PD_BULLET; break;
	case LD_NUMBER: d = PD_NUMBER; break;
	case LD_RULE: d = PD_RULE; break;
	case LD_HEAD_RULE: d = PD_HEAD_RULE; break;
	}
	if (ln->kind == BK_QUOTE)
		d |= PD_QUOTE;
	return d;
}

/* encode lines [a, b) as one page (DESIGN.md 4.2) */
/* with note: a last line, centred at the bottom, saying why the page
 * ends early (the picture after it did not fit) */
#define NOTE_PIC "\xe2\x96\xbc Picture on the next page \xe2\x96\xbc"
#define NOTE_FONT FONT_SMALLL

static void encode_page(buf_t *pg, const layout_t *l, const doc_t *d, const uint32_t *linkmap,
			uint32_t a, uint32_t b, const char *note)
{
	uint32_t slots[255];
	unsigned nslots = 0, i;
	int32_t top = l->lines[a].y;
	pg->n = 0;
	b_u8(pg, b - a + (note != NULL));
	for (i = a; i < b; ++i) {
		const lay_line_t *ln = &l->lines[i];
		unsigned deco = deco_code(ln), k, nitems = 0;
		int32_t y = ln->y - top;
		b_u16(pg, (unsigned)(y < 0 ? 0 : y));
		b_u8(pg, ln->h > 255 ? 255 : ln->h);
		b_u8(pg, ln->base > 255 ? 255 : ln->base);
		b_u8(pg, deco);
		if ((deco & 0x7f) == PD_BULLET || (deco & 0x7f) == PD_NUMBER) {
			b_u16(pg, (uint16_t)ln->deco_x);
			b_u16(pg, ln->number);
		}
		for (k = 0; k < ln->nfrag; ++k)
			if (l->frags[ln->first + k].len)
				nitems++;
		if (ln->deco == LD_PIC && ln->number < d->pics.n)
			nitems++;
		b_u8(pg, nitems > 255 ? 255 : nitems);
		nitems = 0;
		if (ln->deco == LD_PIC && ln->number < d->pics.n) {
			/* a picture: x, w, h, its number in NAME.I00 */
			unsigned pk = ln->number;
			b_u8(pg, PI_PIC);
			b_varint(pg, 10);
			b_u16(pg, (uint16_t)ln->deco_x);
			b_u16(pg, d->pics.w[pk]);
			b_u16(pg, d->pics.h[pk]);
			b_u16(pg, (uint16_t)d->pics.off[pk]);
			b_u16(pg, (uint16_t)(d->pics.off[pk] >> 16));
			nitems++;
		}
		for (k = 0; k < ln->nfrag && nitems < 255; ++k) {
			const lay_frag_t *f = &l->frags[ln->first + k];
			unsigned slot = PI_NOLINK, s;
			if (!f->len)
				continue;
			nitems++;
			if (f->link != 0xffff && f->link < d->nlinks && linkmap[f->link] != TOC_NONE) {
				for (s = 0; s < nslots; ++s)
					if (slots[s] == linkmap[f->link])
						break;
				if (s == nslots && nslots < 254)
					slots[nslots++] = linkmap[f->link];
				if (s < nslots)
					slot = s;
			}
			b_u8(pg, f->font);
			b_u16(pg, (uint16_t)f->x);
			b_u8(pg, (uint8_t)f->dy);
			b_u8(pg, slot);
			b_varint(pg, f->len);
			b_mem(pg, d->text + f->off, f->len);
		}
	}
	if (note) {
		size_t n = strlen(note);
		int w = font_width(NOTE_FONT, note, n);
		b_u16(pg, PAGE_H - 17);
		b_u8(pg, 16);
		b_u8(pg, 12);
		b_u8(pg, PD_NONE);
		b_u8(pg, 1);
		b_u8(pg, NOTE_FONT);
		b_u16(pg, (uint16_t)((PAGE_W - w) / 2));
		b_u8(pg, 0);
		b_u8(pg, PI_NOLINK);
		b_varint(pg, (uint32_t)n);
		b_mem(pg, note, n);
	}
	b_u8(pg, nslots);
	for (i = 0; i < nslots; ++i)
		b_varint(pg, slots[i]);
}

static int is_blank(const lay_line_t *ln)
{
	return ln->nfrag == 0 && ln->deco == LD_NONE;	/* a picture line is not */
}

static int g_pics = PICS_ALL;	/* PICS_NONE, PICS_LEAD, PICS_ALL (1.0.0: all) */
static int g_colour = 1, g_jpeg_q = 75;	/* --grey, --jpeg-quality (1.0.0: colour) */
static int g_pic_h = 255;		/* --pic-height */

/* lay out article number `art` and turn it into frames.  With raw set,
 * the frames are left uncompressed (dictionary samples). */
static void do_article(worker_t *w, uint32_t art, art_out_t *o, int raw)
{
	zim_t *z = &w->z;
	zim_dirent_t de;
	const uint8_t *p;
	uint32_t len, i, nheads = 0, hb = 0, page = 0, *heads = NULL, *linkmap;
	doc_t d;
	layout_t l;
	buf_t pg = { 0 }, fr = { 0 };
	pic_set_t ps;

	memset(o, 0, sizeof(*o));
	if (zim_dirent(z, g_arts[art], &de) < 0 || zim_blob(z, de.cluster, de.blob, &p, &len) < 0) {
		o->failed = 1;
		return;
	}
	doc_init(&d);
	if (g_pics) {
		pictures_for(g_arts[art], &ps);
		html_set_pictures(&ps);	/* this thread's; read until the doc is made */
	}
	if (html_to_doc(&d, (const char *)p, len, de.ns, de.url, z->content_ns == 'C') < 0 ||
	    lay_build(&l, &d) < 0) {
		doc_free(&d);
		o->failed = 1;
		return;
	}
	linkmap = resolve_links(z, &d);

	/* where each heading block starts in the text */
	for (i = 0; i < d.nblocks; ++i)
		if (heading_level(d.blocks[i].kind) && d.blocks[i].nruns)
			nheads++;
	heads = xmalloc((nheads + 1) * 2 * sizeof(uint32_t));
	nheads = 0;
	for (i = 0; i < d.nblocks; ++i)
		if (heading_level(d.blocks[i].kind) && d.blocks[i].nruns) {
			heads[nheads * 2] = d.runs[d.blocks[i].first_run].start;
			heads[nheads * 2 + 1] = i;
			nheads++;
		}

	o->fsize = xmalloc((l.nlines + 1) * sizeof(uint32_t));
	i = 0;
	while (i < l.nlines) {
		uint32_t j, k;
		int32_t top;
		while (i < l.nlines && is_blank(&l.lines[i]))
			i++;
		if (i >= l.nlines)
			break;
		top = l.lines[i].y;
		j = i;
		while (j < l.nlines && l.lines[j].y + l.lines[j].h - top <= PAGE_H)
			j++;
		if (j == i)
			j = i + 1;	/* taller than a page: on its own */
		/* never leave a heading at the bottom of a page, nor a picture
		 * apart from its caption (it goes to the next page with it) */
		if (j < l.nlines) {
			uint32_t j0 = j;
			while (j - 1 > i) {
				const lay_line_t *prev = &l.lines[j - 1];
				if (prev->deco == LD_PIC && l.lines[j].kind == BK_PICCAP) {
					j--;	/* the picture goes with its caption */
					continue;
				}
				/* the next page starts with a picture: a heading may end
				 * this one (moving it on would only leave a gap) */
				if (l.lines[j].deco == LD_PIC)
					break;
				if (heading_level(prev->kind) || prev->kind == BK_TITLE || is_blank(prev)) {
					j--;
					continue;
				}
				break;
			}
			/* but not down to a page of nothing but a heading (a heading
			 * followed by a picture too tall to share the page): then the
			 * whole heading stays here and the picture goes on */
			for (k = i; k < j; ++k)
				if (!heading_level(l.lines[k].kind) && l.lines[k].kind != BK_TITLE &&
				    !is_blank(&l.lines[k]))
					break;
			if (k == j) {
				j = i;
				while (j < j0 && (heading_level(l.lines[j].kind) ||
						  l.lines[j].kind == BK_TITLE || is_blank(&l.lines[j])))
					j++;
				if (j == i)
					j = i + 1;
			}
		}
		/* sections that start on this page */
		for (k = i; k < j; ++k) {
			const lay_line_t *ln = &l.lines[k];
			while (hb < nheads && ln->nfrag && l.frags[ln->first].off > heads[hb * 2])
				hb++;	/* heading not laid out (empty) */
			if (hb < nheads && ln->nfrag && l.frags[ln->first].off == heads[hb * 2]) {
				const doc_block_t *bl = &d.blocks[heads[hb * 2 + 1]];
				char t[256];
				size_t tl = 0, r;
				for (r = 0; r < bl->nruns && tl < 255; ++r) {
					const doc_run_t *ru = &d.runs[bl->first_run + r];
					size_t n = ru->len;
					if (tl + n > 255)
						n = 255 - tl;
					memcpy(t + tl, d.text + ru->start, n);
					tl += n;
				}
				while (tl && (t[tl - 1] & 0xc0) == 0x80)	/* cut whole chars */
					tl--;
				if (tl && (uint8_t)t[tl - 1] >= 0xc0)
					tl--;
				b_u16(&o->sec, page);
				b_u8(&o->sec, heading_level(bl->kind));
				b_u8(&o->sec, tl);
				b_mem(&o->sec, t, tl);
				if (o->nsec < 0xffff)
					o->nsec++;
				hb++;
			}
		}
		{
			/* ended early for a picture that did not fit: say so */
			const char *note = NULL;
			if (j < l.nlines && j > i && l.lines[j].deco == LD_PIC &&
			    PAGE_H - (l.lines[j - 1].y + l.lines[j - 1].h - top) >= 20)
				note = NOTE_PIC;
			encode_page(&pg, &l, &d, linkmap, i, j, note);
		}
		/* frame: one page */
		fr.n = 0;
		b_u8(&fr, 1);
		b_u16(&fr, pg.n);
		b_mem(&fr, pg.p, pg.n);
		if (fr.n > FRAME_MAX)
			die("a page is too large in %s", de.url);
		o->raw += fr.n;
		if (raw) {
			b_mem(&o->frames, fr.p, fr.n);
			o->fsize[o->nframes++] = fr.n;
		} else {
			size_t cap = ZSTD_compressBound(fr.n), c;
			b_need(&o->frames, cap);
			c = ZSTD_compress_usingCDict(w->cctx, o->frames.p + o->frames.n, cap,
						     fr.p, fr.n, g_cdict);
			if (ZSTD_isError(c))
				die("zstd: %s", ZSTD_getErrorName(c));
			o->frames.n += c;
			o->fsize[o->nframes++] = (uint32_t)c;
		}
		page++;
		i = j;
		if (page >= 0xffff)
			break;
	}
	free(pg.p);
	free(fr.p);
	free(heads);
	free(linkmap);
	lay_free(&l);
	doc_free(&d);
}

static void out_free(art_out_t *o)
{
	free(o->frames.p);
	free(o->fsize);
	free(o->sec.p);
	memset(o, 0, sizeof(*o));
}

/* ------------------------------------------------------------------ */
/* workers */

static worker_t *g_w;
static int g_nthreads;
static art_out_t *g_out;	/* results of the current batch */
static uint32_t g_batch_first, g_batch_n;
static uint32_t *g_batch_list;	/* or: these article numbers (samples) */
static int g_raw;
static uint32_t g_next;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void *work(void *arg)
{
	worker_t *w = arg;
	for (;;) {
		uint32_t a, k;
		pthread_mutex_lock(&g_lock);
		a = g_next;
		g_next += GRAB;
		pthread_mutex_unlock(&g_lock);
		if (a >= g_batch_n)
			break;
		for (k = a; k < a + GRAB && k < g_batch_n; ++k)
			do_article(w, g_batch_list ? g_batch_list[k] : g_batch_first + k, &g_out[k], g_raw);
	}
	return NULL;
}

static void run_batch(void)
{
	int t;
	g_next = 0;
	for (t = 0; t < g_nthreads; ++t)
		if (pthread_create(&g_w[t].th, NULL, work, &g_w[t]) != 0)
			die("cannot start a thread", "");
	for (t = 0; t < g_nthreads; ++t)
		pthread_join(g_w[t].th, NULL);
}

/* ------------------------------------------------------------------ */
/* title index (Gen2 format, target = article number) */

typedef struct rec {
	uint32_t key, title, target;
	uint8_t flags, tlen;
} rec_t;

typedef struct pool {
	char *p;
	size_t len, cap;
} pool_t;

static pool_t keys, titles;
static rec_t *recs;
static size_t nrec, caprec;

static uint32_t pool_add(pool_t *p, const char *s, size_t n)
{
	size_t off = p->len;
	if (p->len + n + 1 > p->cap) {
		p->cap = (p->cap + n + 1) * 2;
		p->p = xrealloc(p->p, p->cap);
	}
	memcpy(p->p + p->len, s, n);
	p->p[p->len + n] = 0;
	p->len += n + 1;
	if (p->len > 0xffffffffu)
		die("title pool exceeds 4 GiB", "");
	return (uint32_t)off;
}

static int rec_cmp(const void *a, const void *b)
{
	const rec_t *x = a, *y = b;
	int c = strcmp(keys.p + x->key, keys.p + y->key);
	if (c)
		return c;
	if (x->flags != y->flags)
		return (x->flags & IDX_FLAG_REDIRECT) ? 1 : -1;	/* articles first */
	c = strcmp(titles.p + x->title, titles.p + y->title);
	if (c)
		return c;
	return x->target < y->target ? -1 : x->target > y->target;
}

static size_t clip(const char *s)
{
	size_t n = strlen(s);
	if (n <= 255)
		return n;
	n = 255;
	while (n && ((unsigned char)s[n] & 0xc0) == 0x80)
		n--;
	return n;
}

/* a title record; with a section (sec not empty) the section name is
 * kept after the title and a NUL (IDX_FLAG_SECTION), as long as both
 * fit the 255 bytes: firmware before 1.0.0 shows the title and stops */
static void add_record_sec(const char *title, const char *sec, uint32_t target, uint8_t flags)
{
	char key[512], both[256];
	size_t tl = clip(title), kl, sl = sec ? strlen(sec) : 0;
	if (!tl || target == TOC_NONE)
		return;
	kl = fold_utf8(title, tl, key, sizeof(key));
	if (!kl)
		return;
	if (sl && tl + 1 + sl <= 255) {
		memcpy(both, title, tl);
		both[tl] = 0;
		memcpy(both + tl + 1, sec, sl);
		title = both;
		tl += 1 + sl;
		flags |= IDX_FLAG_SECTION;
	}
	if (nrec == caprec) {
		caprec = caprec ? caprec * 2 : 1 << 16;
		recs = xrealloc(recs, caprec * sizeof(rec_t));
	}
	recs[nrec].key = pool_add(&keys, key, kl);
	recs[nrec].title = pool_add(&titles, title, tl);
	recs[nrec].tlen = (uint8_t)tl;
	recs[nrec].target = target;
	recs[nrec].flags = flags;
	nrec++;
}

static void add_record(const char *title, uint32_t target, uint8_t flags)
{
	add_record_sec(title, NULL, target, flags);
}

/* ------------------------------------------------------------------ */
/* section redirects: Kiwix keeps a redirect to a section ("Croaking" to
 * Frog#Call), which a ZIM redirect cannot hold, as a tiny page:
 *
 *   <html><head><title>T</title>
 *   <meta http-equiv="refresh" content="0;URL='./Frog#Call'" />
 *   </head><body><a href="./Frog#Call">T</a></body></html>
 *
 * Such a page (under 2 KB, one refresh with no delay, a body of just one
 * link to the same place) is made a redirect record to its target, with
 * the section, instead of a one-line article.  Links to it go to the
 * target too. */

static void progress(const char *what, uint64_t done, uint64_t total);

#define STUB_MAX 2048
static uint32_t *g_stub_to;	/* per entry: target entry, TOC_NONE if not a stub */
static uint32_t *g_stub_sec;	/* per entry: section offset in g_secs */
static pool_t g_secs;
static pthread_mutex_t g_stub_mx = PTHREAD_MUTEX_INITIALIZER;

typedef struct cand {
	uint32_t cluster, blob, entry;
} cand_t;

static int cand_cmp(const void *a, const void *b)
{
	const cand_t *x = a, *y = b;
	if (x->cluster != y->cluster)
		return x->cluster < y->cluster ? -1 : 1;
	return x->blob < y->blob ? -1 : x->blob > y->blob;
}

static const char *find_n(const char *h, size_t n, const char *needle)
{
	size_t k = strlen(needle), i;
	for (i = 0; i + k <= n; ++i)
		if (memcmp(h + i, needle, k) == 0)
			return h + i;
	return NULL;
}

static int count_n(const char *h, size_t n, const char *needle)
{
	int c = 0;
	const char *e = h + n, *q = h;
	while ((q = find_n(q, (size_t)(e - q), needle))) {
		c++;
		q++;
	}
	return c;
}

static int hexv(int c)
{
	return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
	       c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* the page at entry url, if a section redirect stub: its target address
 * (as written, e.g. "./Frog#Call") into to; 0 if a stub */
static int stub_target(const char *h, size_t n, char *to, size_t cap)
{
	const char *m, *u, *end, *q, *b, *be, *a;
	size_t tl;
	if (n >= STUB_MAX || count_n(h, n, "http-equiv=\"refresh\"") != 1 || count_n(h, n, "<a ") != 1)
		return -1;
	m = find_n(h, n, "http-equiv=\"refresh\"");
	if (!(u = find_n(m, (size_t)(h + n - m), "content=\"0;URL='")))
		return -1;
	u += strlen("content=\"0;URL='");
	if (!(end = find_n(u, (size_t)(h + n - u), ">")))
		return -1;
	/* the address ends at the last ' before the tag's end (it may hold
	 * quotes itself) */
	for (q = end; q > u && *q != '\''; --q)
		;
	if (q <= u || (tl = (size_t)(q - u)) >= cap)
		return -1;
	memcpy(to, u, tl);
	to[tl] = 0;
	/* the body: one link to the same place, nothing else */
	if (!(b = find_n(h, n, "<body>")) || !(be = find_n(b, (size_t)(h + n - b), "</body>")))
		return -1;
	b += 6;
	while (b < be && (*b == ' ' || *b == '\n' || *b == '\r' || *b == '\t'))
		b++;
	while (be > b && (be[-1] == ' ' || be[-1] == '\n' || be[-1] == '\r' || be[-1] == '\t'))
		be--;
	a = b;
	if ((size_t)(be - a) < 9 + tl + 2 + 4 || memcmp(a, "<a href=\"", 9) != 0 ||
	    memcmp(a + 9, to, tl) != 0 || memcmp(a + 9 + tl, "\">", 2) != 0 ||
	    memcmp(be - 4, "</a>", 4) != 0)
		return -1;
	/* no tags in the link text */
	for (q = a + 9 + tl + 2; q < be - 4; ++q)
		if (*q == '<')
			return -1;
	return 0;
}

/* address relative to entry url -> ZIM path (percent decoded, . and ..
 * resolved) and section (percent decoded, '_' as space) */
static int stub_split(const char *url, const char *to, char *path, size_t pcap, char *sec, size_t scap)
{
	char dec[1024], *seg[64], *q;
	size_t o = 0, i, at = 0;
	int nseg = 0, k;
	const char *hash = strchr(to, '#');
	size_t pl = hash ? (size_t)(hash - to) : strlen(to);
	sec[0] = 0;
	if (hash) {
		size_t so = 0;
		for (i = 1; hash[i] && so + 1 < scap; ++i) {
			if (hash[i] == '%' && hexv(hash[i + 1]) >= 0 && hexv(hash[i + 2]) >= 0) {
				sec[so++] = (char)(hexv(hash[i + 1]) * 16 + hexv(hash[i + 2]));
				i += 2;
			} else {
				sec[so++] = hash[i] == '_' ? ' ' : hash[i];
			}
		}
		sec[so] = 0;
	}
	/* base directory of the stub, then the address */
	q = strrchr(url, '/');
	if (q && to[0] != '/') {
		at = (size_t)(q - url) + 1;
		if (at >= sizeof(dec))
			return -1;
		memcpy(dec, url, at);
	}
	o = at;
	for (i = to[0] == '/' ? 1 : 0; i < pl && o + 1 < sizeof(dec); ++i) {
		if (to[i] == '%' && i + 2 < pl && hexv(to[i + 1]) >= 0 && hexv(to[i + 2]) >= 0) {
			dec[o++] = (char)(hexv(to[i + 1]) * 16 + hexv(to[i + 2]));
			i += 2;
		} else {
			dec[o++] = to[i];
		}
	}
	dec[o] = 0;
	for (q = dec; *q && nseg < 64;) {
		char *st = q;
		while (*q && *q != '/')
			q++;
		if (*q)
			*q++ = 0;
		if (!*st || strcmp(st, ".") == 0)
			continue;
		if (strcmp(st, "..") == 0) {
			if (nseg)
				nseg--;
			continue;
		}
		seg[nseg++] = st;
	}
	if (!nseg)
		return -1;
	o = 0;
	for (k = 0; k < nseg; ++k) {
		size_t l = strlen(seg[k]);
		if (o + l + 2 > pcap)
			return -1;
		if (k)
			path[o++] = '/';
		memcpy(path + o, seg[k], l);
		o += l;
	}
	path[o] = 0;
	return 0;
}

typedef struct stub_job {
	pthread_t th;
	const cand_t *c;
	uint32_t n;
	zim_t z;
	host_files_t files;
	uint32_t found;
} stub_job_t;

static volatile uint32_t g_stub_done;
static volatile int g_stub_left;	/* threads still going */
static uint32_t g_nstubs;

static void *stub_work(void *arg)
{
	stub_job_t *j = arg;
	uint32_t i;
	char to[1024], path[1024], sec[256];
	for (i = 0; i < j->n; ++i) {
		const uint8_t *data;
		uint32_t len, t, fin;
		zim_dirent_t d, fd;
		if (i % 4096 == 0)
			__sync_fetch_and_add(&g_stub_done, i ? 4096 : 0);
		if (zim_blob(&j->z, j->c[i].cluster, j->c[i].blob, &data, &len) < 0 ||
		    stub_target((const char *)data, len, to, sizeof(to)) < 0)
			continue;
		if (zim_dirent(&j->z, j->c[i].entry, &d) < 0 ||
		    stub_split(d.url, to, path, sizeof(path), sec, sizeof(sec)) < 0 ||
		    zim_find(&j->z, j->z.content_ns, path, &t) < 0 ||
		    zim_follow(&j->z, t, &fin, &fd) < 0 || fd.ns != j->z.content_ns ||
		    !is_html(&j->z, fd.mime) || fin >= j->z.entry_count || fin == j->c[i].entry)
			continue;
		pthread_mutex_lock(&g_stub_mx);
		g_stub_to[j->c[i].entry] = fin;
		g_stub_sec[j->c[i].entry] = sec[0] ? pool_add(&g_secs, sec, strlen(sec)) : 0xffffffffu;
		pthread_mutex_unlock(&g_stub_mx);
		j->found++;
	}
	__sync_fetch_and_sub(&g_stub_left, 1);
	return NULL;
}

/* find the stubs: every small HTML page is read, in the order the ZIM
 * stores them (each cluster decoded once), by all threads */
static void find_stubs(zim_t *z, const char *input)
{
	cand_t *c = xmalloc((size_t)z->entry_count * sizeof(cand_t));
	uint32_t n = 0, i, per, found = 0, chained = 0, k;
	stub_job_t *jobs;
	int t;
	g_stub_to = xmalloc((size_t)z->entry_count * sizeof(uint32_t));
	g_stub_sec = xmalloc((size_t)z->entry_count * sizeof(uint32_t));
	for (i = 0; i < z->entry_count; ++i) {
		zim_dirent_t d;
		g_stub_to[i] = TOC_NONE;
		g_stub_sec[i] = 0xffffffffu;
		if (zim_dirent(z, i, &d) < 0 || d.ns != z->content_ns || d.mime == ZIM_MIME_REDIRECT ||
		    !is_html(z, d.mime))
			continue;
		c[n].cluster = d.cluster;
		c[n].blob = d.blob;
		c[n].entry = i;
		n++;
	}
	qsort(c, n, sizeof(cand_t), cand_cmp);
	jobs = xmalloc((size_t)g_nthreads * sizeof(stub_job_t));
	memset(jobs, 0, (size_t)g_nthreads * sizeof(stub_job_t));
	per = (n + (uint32_t)g_nthreads - 1) / (uint32_t)g_nthreads;
	g_stub_done = 0;
	g_stub_left = g_nthreads;
	for (t = 0; t < g_nthreads; ++t) {
		uint32_t from = (uint32_t)t * per;
		jobs[t].c = c + (from < n ? from : n);
		jobs[t].n = from < n ? (n - from < per ? n - from : per) : 0;
		if (host_files_open(&jobs[t].files, input) < 0 || zim_open(&jobs[t].z, &jobs[t].files.io) < 0)
			die("cannot read %s", input);
		if (pthread_create(&jobs[t].th, NULL, stub_work, &jobs[t]) != 0)
			die("cannot start a thread", "");
	}
	while (__sync_fetch_and_add(&g_stub_left, 0)) {
		progress("finding section redirects", __sync_fetch_and_add(&g_stub_done, 0), n);
		usleep(200000);
	}
	for (t = 0; t < g_nthreads; ++t) {
		pthread_join(jobs[t].th, NULL);
		found += jobs[t].found;
		zim_close(&jobs[t].z);
		host_files_close(&jobs[t].files);
	}
	progress("finding section redirects", n, n);
	/* a stub to a stub: on to the end (its own section if it has one);
	 * a loop or more than 8 steps: not a stub after all */
	for (i = 0; i < z->entry_count; ++i) {
		uint32_t to = g_stub_to[i];
		if (to == TOC_NONE || g_stub_to[to] == TOC_NONE)
			continue;
		for (k = 0; k < 8 && g_stub_to[to] != TOC_NONE; ++k) {
			if (g_stub_sec[i] == 0xffffffffu)
				g_stub_sec[i] = g_stub_sec[to];
			to = g_stub_to[to];
		}
		if (g_stub_to[to] != TOC_NONE || to == i) {
			g_stub_to[i] = TOC_NONE;
			found--;
		} else {
			g_stub_to[i] = to;
			chained++;
		}
	}
	free(c);
	free(jobs);
	g_nstubs = found;
	(void)chained;
}

static void write_index(const char *path, const char *name, uint32_t main_art,
			uint32_t pieces, uint64_t data_len)
{
	FILE *f = fopen(path, "wb");
	uint8_t page[IDX_PAGE], hdr[IDX_PAGE];
	uint8_t *top;
	size_t i, pages = 0, cap_pages = 1024, off = 0, articles = 0, out = 0;
	char meta[256];

	if (!f)
		die("cannot write %s", path);
	top = xmalloc(cap_pages * IDX_TOP);
	memset(hdr, 0, sizeof(hdr));
	fwrite(hdr, 1, IDX_PAGE, f);	/* placeholder */
	memset(page, 0, sizeof(page));
	for (i = 0; i < nrec; ++i) {
		rec_t *r = &recs[i];
		size_t need = IDX_REC_HDR + r->tlen;
		if (i && strcmp(keys.p + r->key, keys.p + recs[i - 1].key) == 0 &&
		    r->target == recs[i - 1].target)
			continue;
		if (off == 0 || off + need > IDX_PAGE) {
			if (off) {
				fwrite(page, 1, IDX_PAGE, f);
				memset(page, 0, sizeof(page));
			}
			if (pages == cap_pages) {
				cap_pages *= 2;
				top = xrealloc(top, cap_pages * IDX_TOP);
			}
			{
				uint8_t *t = top + pages * IDX_TOP;
				const char *k = keys.p + r->key;
				size_t kl = strlen(k);
				memset(t, 0, IDX_TOP);
				zw_wr32(t, (uint32_t)out);
				t[4] = (uint8_t)(kl > IDX_KEY ? IDX_KEY : kl);
				memcpy(t + 5, k, t[4]);
			}
			pages++;
			off = 0;
		}
		page[off] = r->tlen;
		page[off + 1] = r->flags;
		zw_wr32(page + off + 2, r->target);
		memcpy(page + off + IDX_REC_HDR, titles.p + r->title, r->tlen);
		off += need;
		out++;
		if (!(r->flags & IDX_FLAG_REDIRECT))
			articles++;
	}
	if (off)
		fwrite(page, 1, IDX_PAGE, f);
	{
		uint64_t top_off = (uint64_t)(pages + 1) * IDX_PAGE;
		uint32_t sum_count = (uint32_t)((pages + IDX_STRIDE - 1) / IDX_STRIDE);
		uint64_t sum_off = top_off + (uint64_t)pages * IDX_TOP;
		if (sum_off + (uint64_t)sum_count * IDX_TOP > 0xffffffffu)
			die("title index larger than 4 GiB", "");
		fwrite(top, IDX_TOP, pages, f);
		for (i = 0; i < sum_count; ++i)
			fwrite(top + i * IDX_STRIDE * IDX_TOP, 1, IDX_TOP, f);
		zw_wr32(hdr + IDX_H_MAGIC, IDX_MAGIC);
		zw_wr32(hdr + IDX_H_VERSION, IDX_VERSION);
		zw_wr32(hdr + IDX_H_PAGES, (uint32_t)pages);
		zw_wr32(hdr + IDX_H_RECORDS, (uint32_t)out);
		zw_wr32(hdr + IDX_H_ARTICLES, (uint32_t)articles);
		zw_wr32(hdr + IDX_H_TOP_OFF, (uint32_t)top_off);
		zw_wr32(hdr + IDX_H_SUM_OFF, (uint32_t)sum_off);
		zw_wr32(hdr + IDX_H_SUM_COUNT, sum_count);
		memcpy(hdr + IDX_H_UUID, g_zim.uuid, 16);
		zw_wr32(hdr + IDX_H_ZIM_ENTRIES, g_narts);
		zw_wr32(hdr + IDX_H_CHUNKS, pieces);
		hdr[IDX_H_CHUNK_SHIFT] = PIECE_SHIFT;
		zw_wr32(hdr + IDX_H_ZIM_SIZE, (uint32_t)data_len);
		zw_wr32(hdr + IDX_H_ZIM_SIZE + 4, (uint32_t)(data_len >> 32));
		zw_wr32(hdr + IDX_H_MAIN, main_art);
		if (zim_metadata(&g_zim, "Title", meta, sizeof(meta)) == 0)
			strncpy((char *)hdr + IDX_H_TITLE, meta, IDX_H_TITLE_LEN - 1);
		if (zim_metadata(&g_zim, "Language", meta, sizeof(meta)) == 0)
			strncpy((char *)hdr + IDX_H_LANG, meta, IDX_H_LANG_LEN - 1);
		strncpy((char *)hdr + IDX_H_NAME, name, IDX_H_NAME_LEN - 1);
		fseeko(f, 0, SEEK_SET);
		fwrite(hdr, 1, IDX_PAGE, f);
	}
	if (fclose(f) != 0)
		die("write failed: %s", path);
	fprintf(stderr, "title index: %zu titles (%zu articles + redirects)\n", out, articles);
	free(top);
}

/* ------------------------------------------------------------------ */
/* page data pieces */

static char g_base[4096];	/* OUTDIR/NAME/NAME */
static FILE *g_piece;
static uint32_t g_npieces;
static uint64_t g_data;		/* bytes written to all pieces */

static void piece_open(uint32_t n)
{
	char path[4200];
	if (g_piece && fclose(g_piece) != 0)
		die("write failed: %s", "page data");
	if (n > 99)
		die("more than 100 pieces%s", "");
	snprintf(path, sizeof(path), "%s.P%02u", g_base, n);
	if (!(g_piece = fopen(path, "wb")))
		die("cannot write %s", path);
	g_npieces = n + 1;
}

/* append a frame; returns its offset in the page data */
static uint64_t piece_put(const uint8_t *p, uint32_t n)
{
	uint64_t piece_size = (uint64_t)1 << PIECE_SHIFT;
	uint64_t in_piece = g_data & (piece_size - 1), off;
	if (!g_piece)
		piece_open(0);
	if (in_piece + n > piece_size) {	/* never across a piece boundary */
		static const uint8_t zero[4096];
		uint64_t pad = piece_size - in_piece;
		while (pad) {
			uint32_t k = pad > sizeof(zero) ? sizeof(zero) : (uint32_t)pad;
			fwrite(zero, 1, k, g_piece);
			pad -= k;
			g_data += k;
		}
	}
	if ((g_data >> PIECE_SHIFT) + 1 > g_npieces)
		piece_open((uint32_t)(g_data >> PIECE_SHIFT));
	off = g_data;
	if (fwrite(p, 1, n, g_piece) != n)
		die("write failed: %s", "page data");
	g_data += n;
	return off;
}

static void placeholder(const char *ext, int kb)
{
	char path[4200];
	FILE *f;
	static const uint8_t zero[1024];
	int i;
	snprintf(path, sizeof(path), "%s.%s", g_base, ext);
	if (!(f = fopen(path, "wb")))
		die("cannot write %s", path);
	for (i = 0; i < kb; ++i)
		fwrite(zero, 1, sizeof(zero), f);
	if (fclose(f) != 0)
		die("write failed: %s", path);
}

static void progress(const char *what, uint64_t done, uint64_t total)
{
	static time_t last;
	time_t now = time(NULL);
	if (now == last && done != total)
		return;
	last = now;
	fprintf(stderr, "\r%s %3d%%", what, total ? (int)(done * 100 / total) : 100);
	if (done == total)
		fprintf(stderr, "\n");
}

/* ------------------------------------------------------------------ */

static void make_name(const char *input, char *out)
{
	const char *b = strrchr(input, '/');
	int n = 0;
	b = b ? b + 1 : input;
	for (; *b && *b != '.' && n < 8; ++b)
		if (isalnum((unsigned char)*b))
			out[n++] = (char)toupper((unsigned char)*b);
	if (!n)
		out[n++] = 'W';
	out[n] = 0;
}

static void mkdirs(const char *dir)
{
	char p[4096];
	size_t k;
	struct stat st;
	snprintf(p, sizeof(p), "%s", dir);
	for (k = 1; p[k]; ++k)
		if (p[k] == '/') {
			p[k] = 0;
			mkdir(p, 0777);
			p[k] = '/';
		}
	mkdir(p, 0777);
	if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode))
		die("cannot make the folder %s", dir);
}

int main(int argc, char **argv)
{
	const char *input = NULL, *outdir = NULL, *font_path = NULL;
	char name[16] = "", dir[4096], path[4200], meta[256];
	int a, dict_kb = 112;
	uint32_t i, main_art = TOC_NONE, nframes = 0, fcap = 1 << 16;
	host_files_t in;
	uint8_t *ftab, *atab, *dict = NULL;
	buf_t sec = { 0 };
	size_t dict_len = 0;
	uint64_t raw_total = 0;
	time_t t0 = time(NULL);
	glyph_t gw;

	for (a = 1; a < argc; ++a) {
		if (strcmp(argv[a], "--version") == 0) {
			printf("zimprep (PicoWikiGuide) %s\n", PWG_VERSION);
			return 0;
		} else if (strcmp(argv[a], "-n") == 0 && a + 1 < argc) {
			const char *s = argv[++a];
			int n = 0;
			for (; *s && n < 8; ++s)
				if (isalnum((unsigned char)*s))
					name[n++] = (char)toupper((unsigned char)*s);
			name[n] = 0;
		} else if (strcmp(argv[a], "--threads") == 0 && a + 1 < argc)
			g_nthreads = atoi(argv[++a]);
		else if (strcmp(argv[a], "--level") == 0 && a + 1 < argc)
			g_level = atoi(argv[++a]);
		else if (strcmp(argv[a], "--dict-kb") == 0 && a + 1 < argc)
			dict_kb = atoi(argv[++a]);
		else if (strcmp(argv[a], "--font") == 0 && a + 1 < argc)
			font_path = argv[++a];
		else if (strcmp(argv[a], "--pictures") == 0 && a + 1 < argc) {
			const char *m = argv[++a];
			g_pics = !strcmp(m, "none") ? PICS_NONE : !strcmp(m, "lead") ? PICS_LEAD :
				 !strcmp(m, "all") ? PICS_ALL : -1;
			if (g_pics < 0)
				die("--pictures takes none, lead or all, not %s", m);
		}
		else if (strcmp(argv[a], "--colour") == 0 || strcmp(argv[a], "--color") == 0)
			g_colour = 1;
		else if (strcmp(argv[a], "--grey") == 0 || strcmp(argv[a], "--gray") == 0)
			g_colour = 0;
		else if (strcmp(argv[a], "--jpeg-quality") == 0 && a + 1 < argc)
			g_jpeg_q = atoi(argv[++a]);
		else if (strcmp(argv[a], "--pic-height") == 0 && a + 1 < argc) {
			g_pic_h = atoi(argv[++a]);
			if (g_pic_h < 24 || g_pic_h > 255)
				die("--pic-height takes 24 to 255, not %s", argv[a]);
		}
		else if (argv[a][0] == '-')
			die("unknown option %s", argv[a]);
		else if (!input)
			input = argv[a];
		else if (!outdir)
			outdir = argv[a];
		else
			die("too many arguments%s", "");
	}
	if (!input || !outdir) {
		fprintf(stderr, "usage: zimprep [-n NAME] [--threads N] [--level N] [--dict-kb N] "
			"[--pictures none|lead|all] [--grey] [--jpeg-quality N] [--pic-height N]\n"
			"       INPUT.zim OUTDIR\n");
		return 2;
	}
	if (!name[0])
		make_name(input, name);
	/* what this run makes, so an old zimprep or a forgotten option shows */
	if (g_pics == PICS_NONE)
		fprintf(stderr, "zimprep (PicoWikiGuide) %s: %s, text only\n", PWG_VERSION, name);
	else
		fprintf(stderr, "zimprep (PicoWikiGuide) %s: %s, pictures %s, %s, height %d%s\n",
			PWG_VERSION, name, g_pics == PICS_LEAD ? "lead" : "all",
			g_colour ? "colour" : "grey", g_pic_h,
			g_colour && g_jpeg_q != 75 ? " (JPEG quality set)" : "");
	if (g_nthreads <= 0)
		g_nthreads = (int)sysconf(_SC_NPROCESSORS_ONLN);
	if (g_nthreads <= 0)
		g_nthreads = 1;
	g_input = input;

	if (host_files_open(&in, input) < 0 || zim_open(&g_zim, &in.io) < 0)
		die("cannot read %s as a ZIM file", input);
	snprintf(dir, sizeof(dir), "%s/%s", outdir, name);
	mkdirs(dir);
	snprintf(g_base, sizeof(g_base), "%s/%s", dir, name);

	/* the Unicode fallback font: widths of other scripts, as on the device */
	{
		char fp[4096];
		FILE *ff;
		long n;
		uint8_t *d;
		if (font_path)
			snprintf(fp, sizeof(fp), "%s", font_path);
		else {
			const char *slash = strrchr(argv[0], '/');
			snprintf(fp, sizeof(fp), "%.*s/../fonts/WIKIFONT.BIN",
				 slash ? (int)(slash - argv[0]) : 1, slash ? argv[0] : ".");
		}
		if (!(ff = fopen(fp, "rb")))
			die("cannot read the Unicode font %s (use --font)", fp);
		fseek(ff, 0, SEEK_END);
		n = ftell(ff);
		fseek(ff, 0, SEEK_SET);
		d = xmalloc((size_t)n);
		if (fread(d, 1, (size_t)n, ff) != (size_t)n || font_set_fallback(d, (uint32_t)n) < 0 ||
		    font_expand_fallback() < 0)	/* the threads share it: no block cache */
			die("%s is not a usable font file", fp);
		fclose(ff);
	}
	html_set_options(HTML_LARGE);	/* no footnotes, no citations */
	lay_set_large(1);
	lay_set_pictures(g_pics != PICS_NONE);
	/* build the font's lazily made bracket pieces before the threads */
	font_glyph(FONT_TEXT, FONT_PIECE, &gw);
	font_glyph(FONT_TEXTL, FONT_PIECE, &gw);

	/* articles and titles */
	find_stubs(&g_zim, input);
	g_entry2art = xmalloc((size_t)g_zim.entry_count * sizeof(uint32_t));
	g_arts = xmalloc((size_t)(g_zim.entry_count + 1) * sizeof(uint32_t));
	for (i = 0; i < g_zim.entry_count; ++i) {
		zim_dirent_t d;
		g_entry2art[i] = TOC_NONE;
		if (zim_dirent(&g_zim, i, &d) < 0 || d.ns != g_zim.content_ns)
			continue;
		if (d.mime != ZIM_MIME_REDIRECT && is_html(&g_zim, d.mime) && g_stub_to[i] == TOC_NONE) {
			g_entry2art[i] = g_narts;
			g_arts[g_narts++] = i;
		}
		progress("reading entries", i, g_zim.entry_count);
	}
	progress("reading entries", g_zim.entry_count, g_zim.entry_count);
	/* a stub stands for its target: links to it go there */
	for (i = 0; i < g_zim.entry_count; ++i)
		if (g_stub_to[i] != TOC_NONE)
			g_entry2art[i] = g_entry2art[g_stub_to[i]];
	if (!g_narts)
		die("no articles in %s", input);
	for (i = 0; i < g_zim.entry_count; ++i) {
		zim_dirent_t d, t;
		uint32_t fin;
		if (zim_dirent(&g_zim, i, &d) < 0 || d.ns != g_zim.content_ns)
			continue;
		if (d.mime == ZIM_MIME_REDIRECT) {
			if (zim_follow(&g_zim, i, &fin, &t) == 0 && fin < g_zim.entry_count)
				add_record_sec(d.title, g_stub_sec[fin] != 0xffffffffu ? g_secs.p + g_stub_sec[fin] : NULL,
					       g_entry2art[fin], IDX_FLAG_REDIRECT);
		} else if (g_stub_to[i] != TOC_NONE) {
			add_record_sec(d.title, g_stub_sec[i] != 0xffffffffu ? g_secs.p + g_stub_sec[i] : NULL,
				       g_entry2art[i], IDX_FLAG_REDIRECT);
		} else if (g_entry2art[i] != TOC_NONE) {
			add_record(d.title, g_entry2art[i], 0);
		}
		progress("reading titles", i, g_zim.entry_count);
	}
	progress("reading titles", g_zim.entry_count, g_zim.entry_count);
	qsort(recs, nrec, sizeof(rec_t), rec_cmp);
	if (g_zim.main_page != ZIM_NONE) {
		zim_dirent_t d;
		uint32_t fin;
		if (zim_follow(&g_zim, g_zim.main_page, &fin, &d) == 0 && fin < g_zim.entry_count)
			main_art = g_entry2art[fin];
	}
	fprintf(stderr, "%s: %u articles, %zu titles, %u section redirects, %d threads\n", input, g_narts,
		nrec, g_nstubs, g_nthreads);

	/* pictures first: the layout needs their sizes */
	pictures_colour(g_colour, g_jpeg_q);
	pictures_max_height(g_pic_h);
	if (pictures_make(&g_zim, input, g_base, g_pics, g_nthreads) < 0)
		die("could not make the pictures%s", "");
	if (g_pics != PICS_NONE && !pictures_count())
		fprintf(stderr, "no pictures in this ZIM: a text-only wiki\n");

	/* workers */
	g_w = xmalloc(g_nthreads * sizeof(worker_t));
	memset(g_w, 0, g_nthreads * sizeof(worker_t));
	for (a = 0; a < g_nthreads; ++a) {
		if (host_files_open(&g_w[a].files, input) < 0 || zim_open(&g_w[a].z, &g_w[a].files.io) < 0)
			die("cannot read %s", input);
		g_w[a].cctx = ZSTD_createCCtx();
	}
	g_out = xmalloc(BATCH * sizeof(art_out_t));
	memset(g_out, 0, BATCH * sizeof(art_out_t));

	/* dictionary, trained on the pages of a sample of articles */
	if (dict_kb > 112) {
		fprintf(stderr, "zimprep: --dict-kb is at most 112 (the PicoCalc's dictionary buffer)\n");
		return 2;
	}
	if (dict_kb > 0) {
		uint32_t want = g_narts < 4000 ? g_narts : 4000, k, nsamp = 0, scap = 1 << 16;
		size_t total = 0, limit = (size_t)dict_kb * 1024 * 100;
		uint8_t *samples = NULL;
		size_t *sizes = xmalloc(scap * sizeof(size_t));
		buf_t sb = { 0 };
		g_batch_list = xmalloc(BATCH * sizeof(uint32_t));
		g_raw = 1;
		for (k = 0; k < want && total < limit; ) {
			uint32_t n = 0, j;
			for (; n < BATCH && k < want; ++n, ++k)
				g_batch_list[n] = (uint32_t)((uint64_t)k * g_narts / want);
			g_batch_n = n;
			run_batch();
			for (j = 0; j < n; ++j) {
				art_out_t *o = &g_out[j];
				size_t off = 0, f;
				for (f = 0; f < o->nframes && total < limit; ++f) {
					if (nsamp == scap) {
						scap *= 2;
						sizes = xrealloc(sizes, scap * sizeof(size_t));
					}
					b_mem(&sb, o->frames.p + off, o->fsize[f]);
					sizes[nsamp++] = o->fsize[f];
					total += o->fsize[f];
					off += o->fsize[f];
				}
				out_free(o);
			}
		}
		samples = sb.p;
		dict = xmalloc((size_t)dict_kb * 1024);
		fprintf(stderr, "training a %d KiB dictionary on %u pages (%.1f MiB)\n",
			dict_kb, nsamp, total / 1048576.0);
		if (nsamp >= 8) {
			dict_len = ZDICT_trainFromBuffer(dict, (size_t)dict_kb * 1024, samples, sizes, nsamp);
			if (ZDICT_isError(dict_len)) {
				fprintf(stderr, "  no dictionary (%s): too little text\n",
					ZDICT_getErrorName(dict_len));
				dict_len = 0;
			}
		}
		free(samples);
		free(sizes);
		free(g_batch_list);
		g_batch_list = NULL;
		g_raw = 0;
	}
	g_cdict = ZSTD_createCDict(dict, dict_len, g_level);
	if (!g_cdict)
		die("zstd: cannot make the dictionary%s", "");

	/* the articles */
	ftab = xmalloc((size_t)fcap * TOC_FRAME_REC);
	atab = xmalloc((size_t)g_narts * TOC_ART_REC);
	for (g_batch_first = 0; g_batch_first < g_narts; g_batch_first += g_batch_n) {
		uint32_t j;
		g_batch_n = g_narts - g_batch_first < BATCH ? g_narts - g_batch_first : BATCH;
		run_batch();
		for (j = 0; j < g_batch_n; ++j) {
			art_out_t *o = &g_out[j];
			uint8_t *ar = atab + (size_t)(g_batch_first + j) * TOC_ART_REC;
			size_t off = 0, f;
			if (o->failed)
				fprintf(stderr, "\nwarning: article %u could not be converted\n",
					g_batch_first + j);
			zw_wr32(ar, nframes);
			zw_wr16(ar + 4, (uint16_t)o->nframes);
			zw_wr16(ar + 6, o->nsec);
			if (o->nsec) {
				if (sec.n > 0xfffffffeu)
					die("section table larger than 4 GiB%s", "");
				zw_wr32(ar + 8, (uint32_t)sec.n);
				b_mem(&sec, o->sec.p, o->sec.n);
			} else {
				zw_wr32(ar + 8, TOC_NONE);
			}
			for (f = 0; f < o->nframes; ++f) {
				uint64_t at = piece_put(o->frames.p + off, o->fsize[f]);
				uint8_t *fe;
				if (nframes == fcap) {
					fcap *= 2;
					ftab = xrealloc(ftab, (size_t)fcap * TOC_FRAME_REC);
				}
				fe = ftab + (size_t)nframes * TOC_FRAME_REC;
				zw_wr32(fe, (uint32_t)at);
				fe[4] = (uint8_t)(at >> 32);
				fe[5] = (uint8_t)o->fsize[f];
				fe[6] = (uint8_t)(o->fsize[f] >> 8);
				fe[7] = (uint8_t)(o->fsize[f] >> 16);
				off += o->fsize[f];
				nframes++;
			}
			raw_total += o->raw;
			out_free(o);
		}
		progress("making pages", g_batch_first + g_batch_n, g_narts);
	}
	if (g_piece && fclose(g_piece) != 0)
		die("write failed: %s", "page data");
	g_piece = NULL;

	/* the TOC */
	{
		uint8_t hdr[TOC_HDR_SIZE];
		FILE *f;
		uint64_t at = TOC_HDR_SIZE;
		uint32_t dict_off, art_off, frame_off, sec_off;
		memset(hdr, 0, sizeof(hdr));
		dict_off = (uint32_t)at;
		at += dict_len;
		art_off = (uint32_t)at;
		at += (uint64_t)g_narts * TOC_ART_REC;
		frame_off = (uint32_t)at;
		at += (uint64_t)nframes * TOC_FRAME_REC;
		sec_off = (uint32_t)at;
		at += sec.n;
		if (at > 0xffffffffu)
			die("the TOC would be larger than 4 GiB%s", "");
		zw_wr32(hdr + TOC_H_MAGIC, TOC_MAGIC);
		zw_wr16(hdr + TOC_H_VERSION, TOC_VERSION);
		zw_wr16(hdr + TOC_H_HDRSIZE, TOC_HDR_SIZE);
		zw_wr32(hdr + TOC_H_MUST, 0);
		zw_wr32(hdr + TOC_H_CONTENT, 0);
		hdr[TOC_H_COMP] = TOC_COMP_ZSTD_DICT;
		hdr[TOC_H_PPF] = 1;
		hdr[TOC_H_TEXTPX] = TEXT_PX;
		hdr[TOC_H_PIECE_SHIFT] = PIECE_SHIFT;
		zw_wr32(hdr + TOC_H_FONTS, font_fingerprint());
		zw_wr16(hdr + TOC_H_PAGE_W, PAGE_W);
		zw_wr16(hdr + TOC_H_PAGE_H, PAGE_H);
		zw_wr32(hdr + TOC_H_ARTICLES, g_narts);
		zw_wr32(hdr + TOC_H_FRAMES, nframes);
		zw_wr32(hdr + TOC_H_PIECES, g_npieces);
		zw_wr32(hdr + TOC_H_MAIN, main_art);
		zw_wr32(hdr + TOC_H_DICT_OFF, dict_off);
		zw_wr32(hdr + TOC_H_DICT_LEN, (uint32_t)dict_len);
		zw_wr32(hdr + TOC_H_ART_OFF, art_off);
		zw_wr32(hdr + TOC_H_FRAME_OFF, frame_off);
		zw_wr32(hdr + TOC_H_SEC_OFF, sec_off);
		zw_wr32(hdr + TOC_H_SEC_LEN, (uint32_t)sec.n);
		zw_wr32(hdr + TOC_H_SEC_LEN + 4, (uint32_t)((uint64_t)sec.n >> 32));
		zw_wr32(hdr + TOC_H_DATA_LEN, (uint32_t)g_data);
		zw_wr32(hdr + TOC_H_DATA_LEN + 4, (uint32_t)(g_data >> 32));
		strncpy((char *)hdr + TOC_H_PREP, PWG_VERSION, TOC_H_PREP_LEN - 1);
		if (zim_metadata(&g_zim, "Title", meta, sizeof(meta)) == 0)
			strncpy((char *)hdr + TOC_H_TITLE, meta, TOC_H_TITLE_LEN - 1);
		if (zim_metadata(&g_zim, "Language", meta, sizeof(meta)) == 0)
			strncpy((char *)hdr + TOC_H_LANG, meta, TOC_H_LANG_LEN - 1);
		strncpy((char *)hdr + TOC_H_NAME, name, TOC_H_NAME_LEN - 1);
		if (zim_metadata(&g_zim, "Date", meta, sizeof(meta)) == 0)
			strncpy((char *)hdr + TOC_H_DATE, meta, TOC_H_DATE_LEN - 1);
		memcpy(hdr + TOC_H_UUID, g_zim.uuid, 16);
		zw_wr32(hdr + TOC_H_PICS, g_pics ? pictures_count() : 0);

		snprintf(path, sizeof(path), "%s.TOC", g_base);
		if (!(f = fopen(path, "wb")))
			die("cannot write %s", path);
		fwrite(hdr, 1, TOC_HDR_SIZE, f);
		fwrite(dict, 1, dict_len, f);
		fwrite(atab, TOC_ART_REC, g_narts, f);
		fwrite(ftab, TOC_FRAME_REC, nframes, f);
		fwrite(sec.p, 1, sec.n, f);
		if (fclose(f) != 0)
			die("write failed: %s", path);
	}
	snprintf(path, sizeof(path), "%s.IDX", g_base);
	write_index(path, name, main_art, g_npieces, g_data);
	placeholder("HIS", 64);
	placeholder("FAV", 32);
	/* the card's state file, next to the wiki folders (kept if there) */
	{
		struct stat st;
		snprintf(path, sizeof(path), "%s/PWGSTATE.DAT", outdir);
		if (stat(path, &st) != 0) {
			FILE *f = fopen(path, "wb");
			static const uint8_t zero[256];
			if (!f || fwrite(zero, 1, sizeof(zero), f) != sizeof(zero) || fclose(f) != 0)
				die("cannot write %s", path);
		}
	}

	fprintf(stderr, "done in %ld s: %u articles, %u pages, %.2f MiB of pages "
		"(%.2f MiB before compression), dictionary %zu bytes\n"
		"copy the folder %s and PWGSTATE.DAT to the top of the SD card\n",
		(long)(time(NULL) - t0), g_narts, nframes, g_data / 1048576.0,
		raw_total / 1048576.0, dict_len, dir);
	return 0;
}
