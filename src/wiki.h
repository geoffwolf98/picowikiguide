/*
 * PicoWikiGuide - reading a prepared wiki: the TOC, pages and sections,
 * and drawing a page.  Works on any zw_io_t (the card's files on the
 * device, plain files on the PC).
 */
#ifndef WIKI_H
#define WIKI_H

#include "zw.h"
#include "card.h"

#define PAGE_MAX_LINES  96
#define PAGE_MAX_ITEMS  480
#define PAGE_MAX_LINKS  255

typedef struct wiki {
	zw_io_t toc, data;
	uint32_t articles, frames, pieces, main;
	uint32_t art_off, frame_off, sec_off;
	uint64_t sec_len, data_len;
	uint32_t fonts;
	char title[TOC_H_TITLE_LEN];
	char name[TOC_H_NAME_LEN];
	char lang[TOC_H_LANG_LEN];
	char date[TOC_H_DATE_LEN];
	uint8_t *dict;
	uint32_t dict_len;
	void *dctx, *ddict;	/* ZSTD_DCtx, ZSTD_DDict */
	uint8_t *cbuf;		/* a compressed frame */
	uint32_t ccap;
	uint8_t *frame;		/* the decompressed frame (FRAME_MAX) */
	uint32_t frame_no;	/* frame in `frame`, TOC_NONE: none */
	uint32_t frame_len;
} wiki_t;

typedef struct wiki_art {
	uint32_t first_frame;
	uint16_t pages;
	uint16_t sections;
	uint32_t sec_off;
} wiki_art_t;

typedef struct page_item {
	const char *text;
	uint16_t len;
	int16_t x;
	int8_t dy;
	uint8_t font;
	uint8_t slot;		/* link slot, PI_NOLINK = none */
	uint8_t line;
} page_item_t;

typedef struct page_line {
	int16_t y;
	uint8_t h, base, deco;
	int16_t deco_x;
	uint16_t number;
	uint16_t first, nitems;	/* items */
} page_line_t;

typedef struct page {
	page_line_t lines[PAGE_MAX_LINES];
	page_item_t items[PAGE_MAX_ITEMS];
	uint32_t links[PAGE_MAX_LINKS];
	uint16_t nlines, nitems, nlinks;
} page_t;

typedef struct wiki_section {
	uint16_t page;
	uint8_t level;
	char title[100];	/* cut to fit (whole characters) */
} wiki_section_t;

/* open a wiki: checks the format, the must-understand flags and the font
 * fingerprint (ZW_EUNSUPPORTED: made for other firmware or fonts) */
int wiki_open(wiki_t *w, const zw_io_t *toc, const zw_io_t *data);
void wiki_close(wiki_t *w);

int wiki_article(wiki_t *w, uint32_t art, wiki_art_t *a);

/* decompress and parse page `page` of an article; the page's text points
 * into w->frame and stays valid until the next wiki_page call */
int wiki_page(wiki_t *w, const wiki_art_t *a, uint32_t page, page_t *pg);

/* section i of an article (reads the TOC each time; cheap enough for
 * a list of a few dozen) */
int wiki_sections(wiki_t *w, const wiki_art_t *a, wiki_section_t *out, uint32_t max);

/* the rectangle of item k (y relative to the body area) */
void page_item_rect(const page_t *pg, uint32_t k, int *x, int *y, int *wd, int *h);

/* draw the page into a SCREEN_STRIDE frame buffer, starting at row y0;
 * focus: an item index whose whole link is shown inverted, or -1 */
void page_draw(const page_t *pg, uint8_t *fb, int y0, int focus);

#endif
