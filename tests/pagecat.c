/*
 * pagecat DIR/NAME "Title" [page] [focus] [-o FILE]
 *                                          draw a page: FILE.png or
 *                                          FILE.pbm (default page.png)
 * pagecat DIR/NAME --check                 decode every page of every
 *                                          article, report problems
 * pagecat DIR/NAME --sections "Title"      list an article's sections
 * pagecat DIR/NAME --index-check           every title (and prefixes of
 *                                          it) found by search, with
 *                                          several summary steps
 * pagecat --font-fingerprint [--patch]     check (or fill in) the
 *                                          fingerprint stored in a
 *                                          compressed fallback font
 *                                          (PWG_WIKIFONT=FILE for --patch)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wiki.h"
#include "index.h"
#include "fold.h"
#include "gfx.h"
#include "font.h"
#include "hostio.h"
#include "plat_common.h"

static int find_title(const char *base, const char *title, uint32_t *art)
{
	host_files_t f;
	char path[4096], key[512];
	idx_t x;
	idx_cursor_t c;
	idx_record_t r;
	size_t kl;
	snprintf(path, sizeof(path), "%s.IDX", base);
	if (host_files_open(&f, path) < 0 || idx_open(&x, &f.io) < 0)
		return -1;
	kl = fold_utf8(title, strlen(title), key, sizeof(key) - 1);
	key[kl] = 0;
	if (idx_seek(&x, key, &c) < 0 || !idx_next(&x, &c, &r))
		return -1;
	{
		char k2[512];
		size_t l2 = fold_utf8(r.title, strlen(r.title), k2, sizeof(k2) - 1);
		if (l2 != kl || memcmp(k2, key, kl) != 0) {
			fprintf(stderr, "no article called \"%s\" (the nearest title is \"%s\")\n",
				title, r.title);
			return -1;
		}
	}
	fprintf(stderr, "found \"%s\" -> article %u%s\n", r.title, r.target,
		(r.flags & IDX_FLAG_REDIRECT) ? " (redirect)" : "");
	*art = r.target;
	return 0;
}

/* every BMP character as the text font draws it */
static uint32_t glyphs_hash(void)
{
	uint32_t h = 2166136261u, cp;
	for (cp = 0; cp < 0x10000; ++cp) {
		glyph_t g;
		uint8_t m[6];
		int i, n;
		font_glyph(FONT_TEXTL, cp, &g);
		m[0] = g.adv; m[1] = (uint8_t)g.xoff; m[2] = (uint8_t)g.top;
		m[3] = g.w; m[4] = g.h; m[5] = g.bpp;
		for (i = 0; i < 6; ++i)
			h = (h ^ m[i]) * 16777619u;
		n = ((g.w * g.bpp + 7) / 8) * g.h;
		for (i = 0; i < n; ++i)
			h = (h ^ g.bits[i]) * 16777619u;
	}
	return h;
}

/* the fingerprint a compressed font stores must be what the same font
 * uncompressed gives (wikis made before the font was compressed keep
 * working); work it out the slow way and compare, or write it in */
static int font_fp(const char *argv0, int patch)
{
	uint32_t h0, sh0, sfinal, full, g0;
	int fmt;
	if (host_load_wikifont(argv0) < 0)
		return 1;
	fmt = font_fallback_info(&h0, &sh0, &sfinal);
	g0 = glyphs_hash();	/* every character, the compressed way */
	if (font_expand_fallback() < 0) {
		fprintf(stderr, "the font does not inflate\n");
		return 1;
	}
	full = font_fingerprint();
	if (glyphs_hash() != g0) {
		printf("the compressed font draws differently from the uncompressed one\n");
		return 1;
	}
	printf("format ZWF%c, built-in fonts %08x, fingerprint %08x, stored %08x %08x\n",
	       fmt == 2 ? '2' : 'T', h0, full, sh0, sfinal);
	if (patch) {
		const char *path = getenv("PWG_WIKIFONT");
		uint8_t b[8];
		FILE *f;
		if (fmt != 2 || !path || !(f = fopen(path, "r+b"))) {
			fprintf(stderr, "--patch needs PWG_WIKIFONT naming a ZWF2 font\n");
			return 1;
		}
		zw_wr32(b, h0);
		zw_wr32(b + 4, full);
		fseek(f, 16, SEEK_SET);
		if (fwrite(b, 1, 8, f) != 8 || fclose(f) != 0)
			return 1;
		printf("written\n");
		return 0;
	}
	if (fmt == 2 && (sh0 != h0 || sfinal != full)) {
		printf("the stored fingerprint is wrong (run with --patch)\n");
		return 1;
	}
	return 0;
}

/* search for every title and some prefixes of it, and seek to every
 * record number, with summary steps that put span edges everywhere;
 * the answers must be what a plain scan gives */
static int index_check(const char *base)
{
	static const uint32_t steps[] = { 1, 2, 3, 5, 7, 128, 512, 4096 };
	char path[4096], key[256];
	host_files_t f;
	idx_t x;
	idx_cursor_t c;
	idx_record_t r;
	char **keys;
	size_t *klen;
	uint32_t n = 0, i, si, bad = 0, seeks = 0;
	snprintf(path, sizeof(path), "%s.IDX", base);
	if (host_files_open(&f, path) < 0 || idx_open(&x, &f.io) < 0) {
		fprintf(stderr, "cannot open %s\n", path);
		return 1;
	}
	keys = calloc(x.records, sizeof(*keys));
	klen = calloc(x.records, sizeof(*klen));
	c.page = 1; c.off = 0; c.number = 0;
	while (n < x.records && idx_next(&x, &c, &r) == 1) {
		klen[n] = fold_utf8(r.title, strlen(r.title), key, sizeof(key));
		keys[n] = malloc(klen[n] + 1);
		memcpy(keys[n], key, klen[n]);
		n++;
	}
	idx_close(&x);
	if (n != x.records) {
		printf("index: %u records read, header says %u\n", n, x.records);
		return 1;
	}
	for (si = 0; si < sizeof(steps) / sizeof(steps[0]); ++si) {
		idx_set_step(steps[si]);
		if (idx_open(&x, &f.io) < 0)
			return 1;
		for (i = 0; i < n; ++i) {
			static const size_t pl[] = { 1, 2, 3, 5, 8, 14, 15, 16, 1000 };
			unsigned k;
			for (k = 0; k < sizeof(pl) / sizeof(pl[0]); ++k) {
				size_t ql = pl[k] < klen[i] ? pl[k] : klen[i];
				uint32_t lo = 0, hi = n;
				char q[257];
				memcpy(q, keys[i], ql);
				q[ql] = 0;
				if (strlen(q) != ql)
					continue;
				while (lo < hi) {	/* first key >= q */
					uint32_t mid = (lo + hi) / 2;
					if (idx_keycmp(keys[mid], klen[mid], q, ql) < 0)
						lo = mid + 1;
					else
						hi = mid;
				}
				seeks++;
				if (idx_seek(&x, q, &c) < 0 || c.number != lo) {
					if (bad++ < 10)
						printf("step %u: \"%s\" gave %u, want %u\n",
						       steps[si], q, c.number, lo);
				}
				if (pl[k] >= klen[i])
					break;
			}
			if (idx_seek_number(&x, i, &c) < 0 || c.number != i ||
			    idx_next(&x, &c, &r) != 1 ||
			    fold_utf8(r.title, strlen(r.title), key, sizeof(key)) != klen[i] ||
			    memcmp(key, keys[i], klen[i]) != 0) {
				if (bad++ < 10)
					printf("step %u: record %u not found by number\n", steps[si], i);
			}
		}
		idx_close(&x);
	}
	idx_set_step(4 * IDX_STRIDE);
	printf("index: %u records, %u pages, %u seeks, %u wrong\n", n, x.pages, seeks, bad);
	return bad != 0;
}

int main(int argc, char **argv)
{
	host_files_t toc, data;
	wiki_t w;
	wiki_art_t a;
	static page_t pg;
	static uint8_t fb[SCREEN_STRIDE * SCREEN_H];
	char path[4096];
	const char *out = "page.png", *pos[5];
	uint32_t art, page = 0;
	int focus = -1, r, ai, npos = 0;
	const char *argv0 = argv[0];

	if (argc >= 2 && strcmp(argv[1], "--font-fingerprint") == 0)
		return font_fp(argv0, argc > 2 && strcmp(argv[2], "--patch") == 0);

	for (ai = 1; ai < argc; ++ai) {
		if (strcmp(argv[ai], "-o") == 0 && ai + 1 < argc)
			out = argv[++ai];
		else if (strcmp(argv[ai], "--check") == 0 || strcmp(argv[ai], "--sections") == 0 ||
			 strcmp(argv[ai], "--index-check") == 0) {
			if (npos < 4)
				pos[npos++] = argv[ai];
		} else if (argv[ai][0] == '-' && argv[ai][1]) {
			fprintf(stderr, "pagecat: unknown option %s\n", argv[ai]);
			return 2;
		} else if (npos < 4) {
			pos[npos++] = argv[ai];
		}
	}
	if (npos < 2) {
		fprintf(stderr, "usage: pagecat DIR/NAME/NAME --check\n"
			"       pagecat DIR/NAME/NAME --sections TITLE\n"
			"       pagecat DIR/NAME/NAME TITLE [PAGE] [FOCUS] [-o FILE.png]\n");
		return 2;
	}
	{
		static char *args[6];
		int i;
		args[0] = "pagecat";
		for (i = 0; i < npos; ++i)
			args[i + 1] = (char *)pos[i];
		argc = npos + 1;
		argv = args;
	}
	if (strcmp(argv[2], "--index-check") == 0)
		return index_check(argv[1]);
	if (host_load_wikifont(argv0) < 0)
		return 1;
	snprintf(path, sizeof(path), "%s.TOC", argv[1]);
	if (host_files_open(&toc, path) < 0 || host_files_open_pieces(&data, argv[1], 'P') < 0) {
		fprintf(stderr, "cannot open %s\n", argv[1]);
		return 1;
	}
	if ((r = wiki_open(&w, &toc.io, &data.io)) < 0) {
		fprintf(stderr, "wiki_open: %d\n", r);
		return 1;
	}
	if (w.pic_count) {	/* its pictures */
		static host_files_t pics;
		if (host_files_open_pieces(&pics, argv[1], 'I') < 0 ||
		    wiki_set_pictures(&w, &pics.io) < 0) {
			fprintf(stderr, "cannot open the pictures %s.I00\n", argv[1]);
			return 1;
		}
	}
	if (strcmp(argv[2], "--check") == 0) {
		uint32_t bad = 0, pages = 0, maxlines = 0, maxitems = 0, maxlinks = 0, headonly = 0, half = 0;
		for (art = 0; art < w.articles; ++art) {
			if (wiki_article(&w, art, &a) < 0) {
				bad++;
				continue;
			}
			for (page = 0; page < a.pages; ++page) {
				pages++;
				if (wiki_page(&w, &a, page, &pg) < 0) {
					fprintf(stderr, "article %u page %u: bad\n", art, page);
					bad++;
					continue;
				}
				/* a page with only a heading on it (not the last) */
				if (page + 1 < a.pages && !pg.npics && pg.nitems) {
					unsigned it;
					for (it = 0; it < pg.nitems; ++it)
						if (pg.items[it].font != FONT_HEADINGL && pg.items[it].font != FONT_TITLEL)
							break;
					if (it == pg.nitems) {
						if (headonly++ < 5)
							fprintf(stderr, "article %u page %u: only a heading\n", art, page + 1);
					}
				}
				/* pages (not the last) less than half full */
				if (page + 1 < a.pages && pg.nlines) {
					/* (not counting the "picture on the next page" note) */
					int last = pg.nlines - 1;
					if (last > 0 && pg.lines[last].y == PAGE_H - 17)
						last--;
					if (pg.lines[last].y + pg.lines[last].h < PAGE_H / 2)
						half++;
				}
				if (pg.nlines > maxlines) maxlines = pg.nlines;
				if (pg.nitems > maxitems) maxitems = pg.nitems;
				if (pg.nlinks > maxlinks) maxlinks = pg.nlinks;
			}
		}
		printf("%u articles, %u pages, %u bad; most per page: %u lines, %u items, %u links\n",
		       w.articles, pages, bad, maxlines, maxitems, maxlinks);
		if (headonly)
			printf("%u pages with only a heading\n", headonly);
		printf("%u pages (not an article's last) less than half full\n", half);
		printf("title \"%s\", name %s, language %s, date %s, main %u\n",
		       w.title, w.name, w.lang, w.date, w.main);
		return bad != 0 || headonly != 0;
	}
	if (strcmp(argv[2], "--sections") == 0 && argc > 3) {
		static wiki_section_t s[1000];
		int n, i;
		if (find_title(argv[1], argv[3], &art) < 0 || wiki_article(&w, art, &a) < 0)
			return 1;
		n = wiki_sections(&w, &a, s, 1000);
		for (i = 0; i < n; ++i)
			printf("%*s%s  (page %u)\n", (s[i].level - 2) * 2, "", s[i].title, s[i].page + 1);
		return 0;
	}
	if (find_title(argv[1], argv[2], &art) < 0) {
		fprintf(stderr, "not found\n");
		return 1;
	}
	if (argc > 3) {
		if (atoi(argv[3]) < 1) {
			fprintf(stderr, "pagecat: the page number must be 1 or more\n");
			return 2;
		}
		page = (uint32_t)atoi(argv[3]) - 1;
	}
	if (argc > 4)
		focus = atoi(argv[4]);
	if (wiki_article(&w, art, &a) < 0)
		return 1;
	if (page >= a.pages) {
		fprintf(stderr, "pagecat: the article has %u pages\n", a.pages);
		return 2;
	}
	if ((r = wiki_page(&w, &a, page, &pg)) < 0) {
		fprintf(stderr, "page: %d\n", r);
		return 1;
	}
	/* top bar */
	gfx_fill(fb, SCREEN_STRIDE, SCREEN_H, 0, 0, SCREEN_W, TOPBAR_H, BLACK);
	font_draw(fb, SCREEN_STRIDE, SCREEN_H, 4, 13, FONT_SMALLL, w.name, strlen(w.name), WHITE);
	{
		char s[32];
		int n = snprintf(s, sizeof(s), "%u / %u", page + 1, a.pages);
		font_draw(fb, SCREEN_STRIDE, SCREEN_H, SCREEN_W - 5 - font_width(FONT_SMALLL, s, n), 13,
			  FONT_SMALLL, s, n, WHITE);
	}
	page_draw(&pg, fb, PAGE_TOP, focus);
	wiki_draw_pictures(&w, &pg, fb, PAGE_TOP);
	fprintf(stderr, "page %u of %u: %u lines, %u items, %u links, %u sections in article\n",
		page + 1, a.pages, pg.nlines, pg.nitems, pg.nlinks, a.sections);
	if (host_save_pbm(out, fb) != 0) {
		fprintf(stderr, "pagecat: cannot write %s\n", out);
		return 1;
	}
	fprintf(stderr, "wrote %s\n", out);
	return 0;
}
