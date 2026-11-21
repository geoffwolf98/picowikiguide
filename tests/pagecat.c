/*
 * pagecat DIR/NAME "Title" [page] [focus] [-o FILE]
 *                                          draw a page: FILE.png or
 *                                          FILE.pbm (default page.png)
 * pagecat DIR/NAME --check                 decode every page of every
 *                                          article, report problems
 * pagecat DIR/NAME --sections "Title"      list an article's sections
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

	for (ai = 1; ai < argc; ++ai) {
		if (strcmp(argv[ai], "-o") == 0 && ai + 1 < argc)
			out = argv[++ai];
		else if (strcmp(argv[ai], "--check") == 0 || strcmp(argv[ai], "--sections") == 0) {
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
	if (strcmp(argv[2], "--check") == 0) {
		uint32_t bad = 0, pages = 0, maxlines = 0, maxitems = 0, maxlinks = 0;
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
				if (pg.nlines > maxlines) maxlines = pg.nlines;
				if (pg.nitems > maxitems) maxitems = pg.nitems;
				if (pg.nlinks > maxlinks) maxlinks = pg.nlinks;
			}
		}
		printf("%u articles, %u pages, %u bad; most per page: %u lines, %u items, %u links\n",
		       w.articles, pages, bad, maxlines, maxitems, maxlinks);
		printf("title \"%s\", name %s, language %s, date %s, main %u\n",
		       w.title, w.name, w.lang, w.date, w.main);
		return bad != 0;
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
	fprintf(stderr, "page %u of %u: %u lines, %u items, %u links, %u sections in article\n",
		page + 1, a.pages, pg.nlines, pg.nitems, pg.nlinks, a.sections);
	if (host_save_pbm(out, fb) != 0) {
		fprintf(stderr, "pagecat: cannot write %s\n", out);
		return 1;
	}
	fprintf(stderr, "wrote %s\n", out);
	return 0;
}
