/*
 * PicoWikiGuide - the application: screens, keys, history, favourites,
 * find, calculator.  Platform independent (see plat.h).
 */
#include <stdio.h>
#include "app.h"
#include "plat.h"
#include "wiki.h"
#include "index.h"
#include "fold.h"
#include "font.h"
#include "gfx.h"
#include "calc.h"
#include "utf8.h"

#define NELEM(a) (sizeof(a) / sizeof((a)[0]))

#define MAX_WIKIS   32
#define BACK_MAX    32
#define QUERY_MAX   64
#define HIST_MAX    60
#define FAV_MAX     100
#define REC_TITLE   64
#define LIST_REC    72	/* u32 article, u16 page, u16 spare, title[64] */
#define LIST_HDR    16
#define FIND_MAX    2000
#define SEC_MAX     300
#define CALC_MAX    60

#define BAR_FONT    FONT_SMALLL
#define ROW_FONT    FONT_TEXTL
#define ROW_H       23
#define LIST_TOP    (TOPBAR_H + 30)
#define LIST_ROWS   ((SCREEN_H - LIST_TOP) / ROW_H)

#define STATE_FILE  "PWGSTATE.DAT"
#define STATE_SIZE  256
#define STATE_MAGIC 0x53475750u	/* "PWGS" */
#define LIST_MAGIC  0x4c475750u	/* "PWGL" */

enum screen {
	S_MSG, S_WIKIS, S_SEARCH, S_ARTICLE, S_SECTIONS, S_LISTS, S_CALC, S_HELP,
};

typedef struct back { uint32_t art; uint16_t page; } back_t;

typedef struct list_ent {
	uint32_t art;
	uint16_t page;
	char title[REC_TITLE];
} list_ent_t;

typedef struct find_hit {
	uint16_t page;
	uint16_t first, last;	/* items */
} find_hit_t;

static struct app {
	uint8_t fb[SCREEN_STRIDE * SCREEN_H];
	int screen, prev_screen;
	char msg[160];

	/* wikis on the card */
	char wikis[MAX_WIKIS][16];
	char wiki_titles[MAX_WIKIS][48];
	int nwikis, wiki_sel;

	/* the open wiki */
	int open;
	char name[16];
	zw_io_t io_toc, io_data, io_idx;
	wiki_t w;
	idx_t idx;

	/* search */
	char query[QUERY_MAX + 1];
	size_t qcur;		/* cursor in query */
	idx_cursor_t top;	/* first row shown */
	int sel;		/* selected row */
	int top_ok;

	/* article */
	int have_art;
	uint32_t art;
	wiki_art_t a;
	uint32_t page;
	page_t pg;
	char title[REC_TITLE * 2];
	int focus;		/* focused item, -1 none */
	back_t back[BACK_MAX];
	int nback;

	/* sections */
	wiki_section_t sec[SEC_MAX];
	int nsec, sec_sel, sec_top;

	/* find */
	int finding;		/* the find bar is shown */
	char fquery[QUERY_MAX + 1];
	size_t fcur;
	find_hit_t hits[FIND_MAX];
	int nhits, hit;		/* hit: current, -1 none */
	int searched;		/* hits are for fquery */

	/* history and favourites */
	list_ent_t hist[HIST_MAX], fav[FAV_MAX];
	int nhist, nfav;
	int list_tab, list_sel, list_top;

	/* "Press F5 for help" in the top bar for the first 10 seconds */
	int hint_on;
	uint32_t hint_t0;

	/* calculator */
	char calc_in[CALC_MAX + 1];
	size_t ccur;
	char calc_out[64];
	double calc_ans;
	int calc_rad;
} A;

/* ------------------------------------------------------------------ */
/* drawing helpers */

static void clear(void)
{
	memset(A.fb, 0, sizeof(A.fb));
}

static int text(int x, int base, int font, const char *s, int colour)
{
	return font_draw(A.fb, SCREEN_STRIDE, SCREEN_H, x, base, font, s, strlen(s), colour);
}

/* how many bytes of s fit in w pixels (whole UTF-8 characters) */
static size_t fit(int font, const char *s, int w)
{
	size_t n = strlen(s);
	while (n && font_width(font, s, n) > w) {
		n--;
		while (n && ((uint8_t)s[n] & 0xc0) == 0x80)
			n--;
	}
	return n;
}

/* text cut to w pixels, with "..." when cut */
static void text_fit(int x, int base, int font, const char *s, int w, int colour)
{
	size_t n = strlen(s);
	if (font_width(font, s, n) > w) {
		static char t[300];
		n = fit(font, s, w - font_width(font, "...", 3));
		if (n > sizeof(t) - 4)
			n = sizeof(t) - 4;
		memcpy(t, s, n);
		memcpy(t + n, "...", 4);
		text(x, base, font, t, colour);
	} else {
		text(x, base, font, s, colour);
	}
}

static void topbar(const char *left, const char *right)
{
	if (A.hint_on)
		left = "Press F5 for help";
	int rw = right ? font_width(BAR_FONT, right, strlen(right)) : 0;
	gfx_fill(A.fb, SCREEN_STRIDE, SCREEN_H, 0, 0, SCREEN_W, TOPBAR_H, BLACK);
	text_fit(4, 13, BAR_FONT, left, SCREEN_W - 14 - rw, WHITE);
	if (right)
		text(SCREEN_W - 5 - rw, 13, BAR_FONT, right, WHITE);
}

static void row(int i, const char *s, const char *right, int selected)
{
	int y = LIST_TOP + i * ROW_H, base = y + 17;
	int rw = right ? font_width(BAR_FONT, right, strlen(right)) : 0;
	text_fit(6, base, ROW_FONT, s, SCREEN_W - 14 - (rw ? rw + 8 : 0), BLACK);
	if (right)
		text(SCREEN_W - 6 - rw, base - 2, BAR_FONT, right, BLACK);
	if (selected)
		gfx_fill(A.fb, SCREEN_STRIDE, SCREEN_H, 2, y, SCREEN_W - 4, ROW_H - 1, INVERT);
}

static void hint(const char *s)
{
	int y = SCREEN_H - 16;
	gfx_hline(A.fb, SCREEN_STRIDE, SCREEN_H, 0, y - 2, SCREEN_W, BLACK);
	text_fit(4, y + 11, BAR_FONT, s, SCREEN_W - 8, BLACK);
}

/* a line of input with a cursor at byte cur */
static void input_box(int y, const char *label, const char *s, size_t cur)
{
	int x = 4, w;
	const char *p = s;
	size_t n, len = strlen(s);
	if (cur > len)
		cur = len;
	if (label)
		x = text(4, y + 16, BAR_FONT, label, BLACK) + 6;
	w = SCREEN_W - x - 6;
	gfx_rect(A.fb, SCREEN_STRIDE, SCREEN_H, x - 3, y, w + 6, 24, BLACK);
	/* scroll so the cursor is in view */
	while (p < s + cur && font_width(ROW_FONT, p, (size_t)(s + cur - p)) > w - 6) {
		p++;
		while (((uint8_t)*p & 0xc0) == 0x80)
			p++;
	}
	n = fit(ROW_FONT, p, w - 2);
	font_draw(A.fb, SCREEN_STRIDE, SCREEN_H, x, y + 18, ROW_FONT, p, n, BLACK);
	x += font_width(ROW_FONT, p, (size_t)(s + cur - p));
	gfx_fill(A.fb, SCREEN_STRIDE, SCREEN_H, x, y + 3, 2, 18, BLACK);
}

/* editing keys for a line of input: Left/Right/Home/End move the cursor,
 * Backspace deletes before it, Del at it (at the end: clears the line),
 * printable keys insert.  Returns 1 when the text changed, 2 when only
 * the cursor moved, 0 when the key is not an editing key */
static int edit_key(char *s, size_t cap, size_t *cur, int key)
{
	size_t len = strlen(s), c = *cur > len ? len : *cur, e;
	switch (key) {
	case K_LEFT:
		if (c) {
			c--;
			while (c && ((uint8_t)s[c] & 0xc0) == 0x80)
				c--;
		}
		*cur = c;
		return 2;
	case K_RIGHT:
		if (c < len) {
			c++;
			while (c < len && ((uint8_t)s[c] & 0xc0) == 0x80)
				c++;
		}
		*cur = c;
		return 2;
	case K_HOME:
		*cur = 0;
		return 2;
	case K_END:
		*cur = len;
		return 2;
	case K_BS:
		if (!c)
			return 2;
		e = c;
		c--;
		while (c && ((uint8_t)s[c] & 0xc0) == 0x80)
			c--;
		memmove(s + c, s + e, len - e + 1);
		*cur = c;
		return 1;
	case K_DEL:
		if (c == len) {
			s[0] = 0;
			*cur = 0;
			return 1;
		}
		e = c + 1;
		while (e < len && ((uint8_t)s[e] & 0xc0) == 0x80)
			e++;
		memmove(s + c, s + e, len - e + 1);
		return 1;
	}
	if (key >= 32 && key < 127) {
		if (len + 1 >= cap)
			return 2;
		memmove(s + c + 1, s + c, len - c + 1);
		s[c] = (char)key;
		*cur = c + 1;
		return 1;
	}
	return 0;
}

static void message(const char *s)
{
	strncpy(A.msg, s, sizeof(A.msg) - 1);
	A.screen = S_MSG;
}

/* ------------------------------------------------------------------ */
/* state, history and favourites files */

static void path_of(char *out, const char *ext)
{
	strcpy(out, A.name);
	strcat(out, "/");
	strcat(out, A.name);
	strcat(out, ".");
	strcat(out, ext);
}

/* reading an article (or a screen opened from one)?  If not, a restart
 * goes back to the search screen rather than the last article */
static int on_article(void)
{
	int s = A.screen;
	if (s == S_HELP || s == S_CALC || s == S_MSG || s == S_LISTS)
		s = A.prev_screen;
	return A.have_art && (s == S_ARTICLE || s == S_SECTIONS);
}

/* PWGSTATE.DAT: u32 magic, name[16], u32 article, u16 page, u8 have
 * article, u8 on search (since 0.5.8), double calc ans, u8 radians,
 * query[64] at 48, u16 selected row at 112 */
static void write_state(const char *name, int keep_art)
{
	uint8_t b[STATE_SIZE];
	memset(b, 0, sizeof(b));
	zw_wr32(b, STATE_MAGIC);
	if (name)
		memcpy(b + 4, name, 16);
	if (keep_art && A.have_art) {
		zw_wr32(b + 20, A.art);
		zw_wr16(b + 24, (uint16_t)A.page);
		b[26] = 1;
		b[27] = !on_article();
	}
	memcpy(b + 32, &A.calc_ans, sizeof(double));
	b[40] = (uint8_t)A.calc_rad;
	if (keep_art) {
		memcpy(b + 48, A.query, QUERY_MAX);
		zw_wr16(b + 112, (uint16_t)A.sel);
	}
	plat_save(STATE_FILE, b, sizeof(b));
}

static void save_state(void)
{
	write_state(A.open ? A.name : NULL, 1);
}

static void load_list(const char *ext, list_ent_t *e, int max, int *n)
{
	char path[40];
	uint8_t *d = NULL;
	uint32_t len = 0, i, cnt;
	*n = 0;
	path_of(path, ext);
	if (plat_load_max(path, LIST_HDR + (uint32_t)max * LIST_REC, &d, &len) < 0)
		return;
	if (len >= LIST_HDR && zw_rd32(d) == LIST_MAGIC) {
		cnt = zw_rd16(d + 4);
		for (i = 0; i < cnt && (int)i < max && LIST_HDR + (i + 1) * LIST_REC <= len; ++i) {
			const uint8_t *r = d + LIST_HDR + i * LIST_REC;
			e[*n].art = zw_rd32(r);
			e[*n].page = zw_rd16(r + 4);
			memcpy(e[*n].title, r + 8, REC_TITLE);
			e[*n].title[REC_TITLE - 1] = 0;
			(*n)++;
		}
	}
	zw_free(d);
}

static void save_list(const char *ext, const list_ent_t *e, int n)
{
	char path[40];
	static uint8_t b[LIST_HDR + FAV_MAX * LIST_REC];
	int i;
	memset(b, 0, sizeof(b));
	zw_wr32(b, LIST_MAGIC);
	zw_wr16(b + 4, (uint16_t)n);
	for (i = 0; i < n; ++i) {
		uint8_t *r = b + LIST_HDR + i * LIST_REC;
		zw_wr32(r, e[i].art);
		zw_wr16(r + 4, e[i].page);
		memcpy(r + 8, e[i].title, REC_TITLE);
	}
	path_of(path, ext);
	plat_save(path, b, LIST_HDR + n * LIST_REC);
}

/* put (art, page) at the front of a list, removing an older copy */
static void list_front(list_ent_t *e, int *n, int max, uint32_t art, uint16_t page, const char *title)
{
	int i, j;
	for (i = 0; i < *n; ++i)
		if (e[i].art == art)
			break;
	if (i == *n) {
		if (*n < max)
			(*n)++;
		i = *n - 1;
	}
	for (j = i; j > 0; --j)
		e[j] = e[j - 1];
	e[0].art = art;
	e[0].page = page;
	strncpy(e[0].title, title, REC_TITLE - 1);
	e[0].title[REC_TITLE - 1] = 0;
}

static int is_fav(uint32_t art)
{
	int i;
	for (i = 0; i < A.nfav; ++i)
		if (A.fav[i].art == art)
			return 1;
	return 0;
}

/* ------------------------------------------------------------------ */
/* opening wikis */

static void close_wiki(void)
{
	if (!A.open)
		return;
	wiki_close(&A.w);
	idx_close(&A.idx);
	plat_close(&A.io_toc);
	plat_close(&A.io_data);
	plat_close(&A.io_idx);
	A.open = 0;
	A.have_art = 0;
	A.nback = 0;
}

static void find_wikis(void)
{
	int i;
	A.nwikis = plat_list_wikis(A.wikis, MAX_WIKIS);
	for (i = 0; i < A.nwikis; ++i) {
		char path[40];
		zw_io_t io;
		uint8_t h[TOC_H_TITLE + 48];
		strcpy(A.wiki_titles[i], A.wikis[i]);
		snprintf(path, sizeof(path), "%s/%s.TOC", A.wikis[i], A.wikis[i]);
		if (plat_open(path, &io) == 0) {
			if (zw_io_read(&io, 0, h, sizeof(h)) == 0 && zw_rd32(h) == TOC_MAGIC && h[TOC_H_TITLE]) {
				memcpy(A.wiki_titles[i], h + TOC_H_TITLE, 47);
				A.wiki_titles[i][47] = 0;
			}
			plat_close(&io);
		}
	}
}

static int open_wiki(const char *name)
{
	char path[40];
	int r;
	close_wiki();
	strncpy(A.name, name, 15);
	A.name[15] = 0;
	snprintf(path, sizeof(path), "%s/%s.TOC", name, name);
	if (plat_open(path, &A.io_toc) < 0) {
		message("Cannot open the wiki's TOC file.");
		return -1;
	}
	snprintf(path, sizeof(path), "%s/%s", name, name);
	if (plat_open_pieces(path, &A.io_data) < 0) {
		plat_close(&A.io_toc);
		message("Cannot open the wiki's page files (.P00).");
		return -1;
	}
	snprintf(path, sizeof(path), "%s/%s.IDX", name, name);
	if (plat_open(path, &A.io_idx) < 0 || idx_open(&A.idx, &A.io_idx) < 0) {
		plat_close(&A.io_toc);
		plat_close(&A.io_data);
		message("Cannot open the wiki's title index (.IDX).");
		return -1;
	}
	if ((r = wiki_open(&A.w, &A.io_toc, &A.io_data)) < 0) {
		idx_close(&A.idx);
		plat_close(&A.io_toc);
		plat_close(&A.io_data);
		plat_close(&A.io_idx);
		message(r == ZW_EUNSUPPORTED ?
			"This wiki was made by a different zimprep or with different fonts. "
			"Prepare it again with the zimprep of this version." :
			r == ZW_ENOMEM ? "Not enough memory to open this wiki." :
			"This wiki's files are damaged or incomplete.");
		return -1;
	}
	A.open = 1;
	load_list("HIS", A.hist, HIST_MAX, &A.nhist);
	load_list("FAV", A.fav, FAV_MAX, &A.nfav);
	A.query[0] = 0;
	A.qcur = 0;
	A.top_ok = 0;
	A.sel = 0;
	return 0;
}

/* ------------------------------------------------------------------ */
/* articles */

/* the article's title: the title line on its first page */
static void read_title(uint32_t art, const wiki_art_t *a, char *out, size_t cap)
{
	size_t n = 0;
	unsigned k;
	out[0] = 0;
	if (!a->pages || wiki_page(&A.w, a, 0, &A.pg) < 0)
		return;
	for (k = 0; k < A.pg.nitems && A.pg.items[k].line == 0; ++k) {
		const page_item_t *it = &A.pg.items[k];
		if (it->font != FONT_TITLEL && it->font != FONT_TITLE)
			continue;
		if (n && n + 1 < cap)
			out[n++] = ' ';
		if (n + it->len >= cap)
			break;
		memcpy(out + n, it->text, it->len);
		n += it->len;
	}
	out[n] = 0;
	(void)art;
}

static int load_page(uint32_t page)
{
	if (page >= A.a.pages)
		page = A.a.pages ? A.a.pages - 1 : 0;
	if (!A.a.pages || wiki_page(&A.w, &A.a, page, &A.pg) < 0) {
		A.pg.nlines = A.pg.nitems = A.pg.nlinks = 0;
		A.page = 0;
		return -1;
	}
	A.page = page;
	A.focus = -1;
	return 0;
}

/* keep the page you are on in the article's History and Favourites
 * entries (in memory; written by save_positions) */
static int fav_dirty, hist_dirty;

static void sync_position(void)
{
	int i;
	if (!A.have_art)
		return;
	for (i = 0; i < A.nhist; ++i)
		if (A.hist[i].art == A.art && A.hist[i].page != A.page) {
			A.hist[i].page = (uint16_t)A.page;
			hist_dirty = 1;
		}
	for (i = 0; i < A.nfav; ++i)
		if (A.fav[i].art == A.art && A.fav[i].page != A.page) {
			A.fav[i].page = (uint16_t)A.page;
			fav_dirty = 1;
		}
}

static void save_positions(void)
{
	sync_position();
	if (hist_dirty)
		save_list("HIS", A.hist, A.nhist);
	if (fav_dirty)
		save_list("FAV", A.fav, A.nfav);
	hist_dirty = fav_dirty = 0;
}

static void remember(void)
{
	if (!A.have_art)
		return;
	sync_position();
	list_front(A.hist, &A.nhist, HIST_MAX, A.art, (uint16_t)A.page, A.title);
}

/* open article art at page; push: keep the current one for Back */
static int open_article(uint32_t art, uint32_t page, int push)
{
	wiki_art_t a;
	if (wiki_article(&A.w, art, &a) < 0)
		return -1;
	if (push && A.have_art) {
		if (A.nback == BACK_MAX) {
			memmove(A.back, A.back + 1, (BACK_MAX - 1) * sizeof(back_t));
			A.nback--;
		}
		A.back[A.nback].art = A.art;
		A.back[A.nback].page = (uint16_t)A.page;
		A.nback++;
	}
	remember();	/* the page we leave, for History */
	A.art = art;
	A.a = a;
	A.have_art = 1;
	read_title(art, &a, A.title, sizeof(A.title));
	if (!A.title[0])
		strcpy(A.title, "(untitled)");
	load_page(page);
	A.finding = 0;
	A.nhits = 0;
	A.hit = -1;
	A.searched = 0;
	A.screen = S_ARTICLE;
	list_front(A.hist, &A.nhist, HIST_MAX, art, (uint16_t)A.page, A.title);
	save_list("HIS", A.hist, A.nhist);
	if (fav_dirty)
		save_list("FAV", A.fav, A.nfav);
	hist_dirty = fav_dirty = 0;
	save_state();
	return 0;
}

/* an entry from History or Favourites: its title decides (article
 * numbers change when a wiki is prepared again) */
static void open_entry(const list_ent_t *e)
{
	static char key[QUERY_MAX * 4];
	size_t kl = fold_utf8(e->title, strlen(e->title), key, sizeof(key) - 1);
	idx_cursor_t c;
	idx_record_t r;
	uint32_t art = e->art;
	key[kl] = 0;
	if (kl && idx_seek(&A.idx, key, &c) == 0 && idx_next(&A.idx, &c, &r)) {
		static char k2[QUERY_MAX * 4];
		size_t l2 = fold_utf8(r.title, strlen(r.title), k2, sizeof(k2) - 1);
		if (l2 == kl && memcmp(k2, key, kl) == 0)
			art = r.target;
	}
	if (art >= A.w.articles || open_article(art, e->page, 1) < 0)
		message("That article is not in this wiki any more.");
}

static void go_back(void)
{
	if (A.nback) {
		back_t b = A.back[--A.nback];
		open_article(b.art, b.page, 0);
	} else {
		A.screen = S_SEARCH;
	}
}

/* links on the page: the first item of each link */
static int link_start(int k)
{
	const page_item_t *it = &A.pg.items[k];
	return it->slot != PI_NOLINK && (k == 0 || A.pg.items[k - 1].slot != it->slot);
}

static void focus_move(int dir)
{
	int k = A.focus;
	for (;;) {
		k = k < 0 ? (dir > 0 ? 0 : (int)A.pg.nitems - 1) : k + dir;
		if (k < 0 || k >= (int)A.pg.nitems)
			return;	/* stay on the last one */
		if (link_start(k)) {
			A.focus = k;
			return;
		}
	}
}

static void follow(void)
{
	uint32_t t;
	if (A.focus < 0 || A.pg.items[A.focus].slot == PI_NOLINK)
		return;
	t = A.pg.links[A.pg.items[A.focus].slot];
	if (t < A.w.articles)
		open_article(t, 0, 1);
}

static void section_jump(int dir)
{
	int n = wiki_sections(&A.w, &A.a, A.sec, SEC_MAX), i;
	uint32_t to = A.page;
	if (dir > 0) {
		for (i = 0; i < n; ++i)
			if (A.sec[i].page > A.page) {
				to = A.sec[i].page;
				break;
			}
	} else {
		to = 0;
		for (i = 0; i < n; ++i)
			if (A.sec[i].page < A.page)
				to = A.sec[i].page;
	}
	A.nsec = n;
	load_page(to);
}

/* ------------------------------------------------------------------ */
/* find in article */

/* fold the text of items [a, b) of a line, keeping where each item
 * starts in the folded text */
static size_t fold_items(const page_t *pg, unsigned a, unsigned b, char *out, size_t cap,
			 uint16_t *starts)
{
	size_t n = 0;
	unsigned k;
	for (k = a; k < b; ++k) {
		const page_item_t *it = &pg->items[k];
		if (k > a) {
			const page_item_t *p = &pg->items[k - 1];
			if (it->x > p->x + font_width(p->font, p->text, p->len) + 1 && n + 1 < cap)
				out[n++] = ' ';
		}
		starts[k - a] = (uint16_t)n;
		n += fold_utf8(it->text, it->len, out + n, cap - n - 1);
	}
	out[n] = 0;
	return n;
}

static void find_all(void)
{
	static char key[QUERY_MAX * 4], line[1200];
	static uint16_t starts[PAGE_MAX_ITEMS];
	size_t kl = fold_utf8(A.fquery, strlen(A.fquery), key, sizeof(key) - 1);
	uint32_t p, keep = A.page;
	A.nhits = 0;
	A.hit = -1;
	A.searched = 1;
	if (!kl)
		return;
	for (p = 0; p < A.a.pages && A.nhits < FIND_MAX; ++p) {
		unsigned l;
		if (wiki_page(&A.w, &A.a, p, &A.pg) < 0)
			continue;
		plat_keepalive();
		for (l = 0; l < A.pg.nlines; ++l) {
			const page_line_t *ln = &A.pg.lines[l];
			size_t n = fold_items(&A.pg, ln->first, ln->first + ln->nitems, line,
					      sizeof(line), starts), at;
			for (at = 0; at + kl <= n && A.nhits < FIND_MAX; ++at) {
				unsigned k, f = ln->first, e;
				if (memcmp(line + at, key, kl) != 0)
					continue;
				for (k = 0; k < ln->nitems; ++k)
					if (starts[k] <= at)
						f = ln->first + k;
				e = f;
				for (k = 0; k < ln->nitems; ++k)
					if (starts[k] < at + kl)
						e = ln->first + k;
				A.hits[A.nhits].page = (uint16_t)p;
				A.hits[A.nhits].first = (uint16_t)f;
				A.hits[A.nhits].last = (uint16_t)e;
				A.nhits++;
				at += kl - 1;
			}
		}
	}
	load_page(keep);
}

static void find_step(int dir)
{
	int i;
	if (!A.searched)
		find_all();
	if (!A.nhits)
		return;
	if (A.hit < 0) {
		/* the first match on or after this page (or before, going up) */
		if (dir > 0) {
			for (i = 0; i < A.nhits && A.hits[i].page < A.page; ++i)
				;
			A.hit = i < A.nhits ? i : 0;
		} else {
			for (i = A.nhits - 1; i >= 0 && A.hits[i].page > A.page; --i)
				;
			A.hit = i >= 0 ? i : A.nhits - 1;
		}
	} else {
		A.hit = (A.hit + dir + A.nhits) % A.nhits;
	}
	if (A.hits[A.hit].page != A.page)
		load_page(A.hits[A.hit].page);
}

/* ------------------------------------------------------------------ */
/* screens: drawing */

static void draw_article(void)
{
	char r[24];
	int barh = 0;
	snprintf(r, sizeof(r), "%s%u / %u", is_fav(A.art) ? "* " : "", A.page + 1, A.a.pages);
	topbar(A.title, r);
	page_draw(&A.pg, A.fb, PAGE_TOP, A.focus);
	if (A.finding && A.hit >= 0 && A.hits[A.hit].page == A.page) {
		unsigned k;
		for (k = A.hits[A.hit].first; k <= A.hits[A.hit].last; ++k) {
			int x, y, w, h;
			const page_item_t *it = &A.pg.items[k];
			const page_line_t *ln = &A.pg.lines[it->line];
			page_item_rect(&A.pg, k, &x, &y, &w, &h);
			/* a find match: light grey behind black text (sharper than inverted) */
			gfx_fill(A.fb, SCREEN_STRIDE, SCREEN_H, x - 1, PAGE_TOP + y, w + 2, h, SHADE);
			font_draw(A.fb, SCREEN_STRIDE, SCREEN_H, it->x, PAGE_TOP + ln->y + ln->base + it->dy,
				  it->font, it->text, it->len, BLACK);
		}
	}
	if (A.finding) {
		char s[48];
		barh = 30;
		gfx_fill(A.fb, SCREEN_STRIDE, SCREEN_H, 0, SCREEN_H - barh, SCREEN_W, barh, WHITE);
		gfx_hline(A.fb, SCREEN_STRIDE, SCREEN_H, 0, SCREEN_H - barh, SCREEN_W, BLACK);
		input_box(SCREEN_H - barh + 3, "Find", A.fquery, A.fcur);
		if (A.searched) {
			if (A.nhits)
				snprintf(s, sizeof(s), "%d/%d", A.hit + 1, A.nhits);
			else
				strcpy(s, "none");
			gfx_fill(A.fb, SCREEN_STRIDE, SCREEN_H, SCREEN_W - 60, SCREEN_H - barh + 4, 54, 22, WHITE);
			text(SCREEN_W - 6 - font_width(BAR_FONT, s, strlen(s)), SCREEN_H - 10, BAR_FONT, s, BLACK);
		}
	}
}

static void draw_search(void)
{
	char t[64];
	idx_cursor_t c;
	idx_record_t r;
	int i;
	snprintf(t, sizeof(t), "Search  %s", A.open ? A.w.title : "");
	topbar(t, A.open ? A.name : NULL);
	input_box(TOPBAR_H + 3, NULL, A.query, A.qcur);
	if (!A.open)
		return;
	if (!A.top_ok) {
		static char key[QUERY_MAX * 4];
		size_t kl = fold_utf8(A.query, strlen(A.query), key, sizeof(key) - 1);
		key[kl] = 0;
		if (idx_seek(&A.idx, key, &A.top) < 0)
			return;
		A.top_ok = 1;
		A.sel = 0;
	}
	c = A.top;
	for (i = 0; i < LIST_ROWS; ++i) {
		if (!idx_next(&A.idx, &c, &r))
			break;
		row(i, r.title, (r.flags & IDX_FLAG_REDIRECT) ? ">" : NULL, i == A.sel);
	}
	if (A.sel >= i)
		A.sel = i ? i - 1 : 0;
}

static void draw_list(const char *title, int n, int sel, int top, const char *(*name)(int),
		      const char *(*right)(int))
{
	int i;
	(void)title;
	for (i = 0; i < LIST_ROWS && top + i < n; ++i)
		row(i, name(top + i), right ? right(top + i) : NULL, top + i == sel);
	if (!n)
		text(6, LIST_TOP + 17, ROW_FONT, "(empty)", BLACK);
}

static const char *wiki_name(int i) { return A.wiki_titles[i]; }
static const char *wiki_folder(int i) { return A.wikis[i]; }
static const char *sec_name(int i) { return A.sec[i].title; }
static const char *sec_page(int i)
{
	static char s[12];
	snprintf(s, sizeof(s), "%u", A.sec[i].page + 1);
	return s;
}
static const char *lst_name(int i)
{
	return A.list_tab ? A.fav[i].title : A.hist[i].title;
}

static void draw_tabs(const char *a, const char *b, int which)
{
	int y = TOPBAR_H + 4, x = 6, w;
	w = font_width(ROW_FONT, a, strlen(a));
	text(x, y + 17, ROW_FONT, a, BLACK);
	if (which == 0)
		gfx_fill(A.fb, SCREEN_STRIDE, SCREEN_H, x - 3, y, w + 6, 22, INVERT);
	x += w + 16;
	w = font_width(ROW_FONT, b, strlen(b));
	text(x, y + 17, ROW_FONT, b, BLACK);
	if (which == 1)
		gfx_fill(A.fb, SCREEN_STRIDE, SCREEN_H, x - 3, y, w + 6, 22, INVERT);
}

static void draw_help(void)
{
	static const char *const lines[] = {
		"F1 Search   F2 History   F3 Random",
		"F4 Calculator   F5 This help",
		"Up/Down  previous / next page",
		"Shift+Up/Down  previous / next section",
		"Left/Right  previous / next link",
		"Enter  follow link    Esc  back",
		"L sections   / find   * favourite",
		"Shift+Tab first page  Shift+Del last",
		"W  choose another wiki",
	};
	char s[80];
	unsigned i;
	topbar("PicoWikiGuide " PWG_VERSION, NULL);
	for (i = 0; i < NELEM(lines); ++i)
		text(6, TOPBAR_H + 20 + i * 19, BAR_FONT, lines[i], BLACK);
	if (A.open) {
		int y = TOPBAR_H + 26 + (int)NELEM(lines) * 19;
		gfx_hline(A.fb, SCREEN_STRIDE, SCREEN_H, 0, y - 12, SCREEN_W, BLACK);
		text_fit(6, y + 4, BAR_FONT, A.w.title, SCREEN_W - 12, BLACK);
		snprintf(s, sizeof(s), "%s, %u articles, %s", A.name, A.w.articles, A.w.date);
		text_fit(6, y + 22, BAR_FONT, s, SCREEN_W - 12, BLACK);
	}
	hint("Esc: back");
}

static void draw_calc(void)
{
	topbar("Calculator", A.calc_rad ? "radians" : "degrees");
	input_box(TOPBAR_H + 10, NULL, A.calc_in, A.ccur);
	if (A.calc_out[0]) {
		int w = font_width(FONT_TITLEL, A.calc_out, strlen(A.calc_out));
		text(SCREEN_W - 8 - (w < SCREEN_W - 16 ? w : SCREEN_W - 16), TOPBAR_H + 70,
		     FONT_TITLEL, A.calc_out, BLACK);
	}
	text(6, TOPBAR_H + 110, BAR_FONT, "+ - * / ^ % ! ( )   E for x10^", BLACK);
	text(6, TOPBAR_H + 128, BAR_FONT, "sin cos tan asin acos atan", BLACK);
	text(6, TOPBAR_H + 146, BAR_FONT, "sqrt ln log   pi e ans", BLACK);
	hint("Enter =   Del at end clears   Tab deg/rad   Esc back");
}

static void draw(void)
{
	clear();
	switch (A.screen) {
	case S_MSG:
		topbar("PicoWikiGuide", NULL);
		{
			/* word wrap the message */
			const char *p = A.msg;
			int y = TOPBAR_H + 30;
			while (*p && y < SCREEN_H - 30) {
				static char ln[160];
				size_t n = fit(ROW_FONT, p, SCREEN_W - 12), m = n;
				if (p[n])
					while (m && p[m] != ' ')
						m--;
				if (!m)
					m = n ? n : 1;
				memcpy(ln, p, m);
				ln[m] = 0;
				text(6, y, ROW_FONT, ln, BLACK);
				p += m;
				while (*p == ' ')
					p++;
				y += ROW_H;
			}
		}
		hint("Esc: back");
		break;
	case S_WIKIS:
		topbar("Choose a wiki", NULL);
		draw_list("", A.nwikis, A.wiki_sel, A.wiki_sel >= LIST_ROWS ? A.wiki_sel - LIST_ROWS + 1 : 0,
			  wiki_name, wiki_folder);
		if (!A.nwikis) {
			clear();
			topbar("PicoWikiGuide", NULL);
			text(6, TOPBAR_H + 30, ROW_FONT, "No wikis on the card.", BLACK);
			text(6, TOPBAR_H + 60, BAR_FONT, "Copy a folder made by zimprep", BLACK);
			text(6, TOPBAR_H + 78, BAR_FONT, "to the top of the SD card.", BLACK);
		}
		break;
	case S_SEARCH:
		draw_search();
		break;
	case S_ARTICLE:
		draw_article();
		break;
	case S_SECTIONS:
		topbar(A.title, "Sections");
		draw_list("", A.nsec, A.sec_sel, A.sec_top, sec_name, sec_page);
		break;
	case S_LISTS:
		topbar("PicoWikiGuide", A.name);
		draw_tabs("History", "Favourites", A.list_tab);
		draw_list("", A.list_tab ? A.nfav : A.nhist, A.list_sel, A.list_top, lst_name, NULL);
		break;
	case S_CALC:
		draw_calc();
		break;
	case S_HELP:
		draw_help();
		break;
	}
	plat_present(A.fb);
}

/* ------------------------------------------------------------------ */
/* screens: keys */

static void list_keys(int key, int n, int *sel, int *top)
{
	if (key == K_DOWN && *sel + 1 < n)
		(*sel)++;
	else if (key == K_UP && *sel > 0)
		(*sel)--;
	else if (key == K_PGDN)
		*sel = *sel + LIST_ROWS < n ? *sel + LIST_ROWS : (n ? n - 1 : 0);
	else if (key == K_PGUP)
		*sel = *sel > LIST_ROWS ? *sel - LIST_ROWS : 0;
	else if (key == K_HOME)
		*sel = 0;
	else if (key == K_END)
		*sel = n ? n - 1 : 0;
	if (*sel < *top)
		*top = *sel;
	if (*sel >= *top + LIST_ROWS)
		*top = *sel - LIST_ROWS + 1;
}

static void search_keys(int key)
{
	idx_cursor_t c;
	idx_record_t r;
	int i;
	if (!A.open) {
		if (key == K_ESC && A.nwikis)
			A.screen = S_WIKIS;
		return;
	}
	if (key == K_DOWN || key == K_PGDN) {
		int steps = key == K_DOWN ? 1 : LIST_ROWS;
		while (steps--) {
			if (A.sel + 1 < LIST_ROWS) {
				/* is there a next row? */
				c = A.top;
				for (i = 0; i <= A.sel + 1; ++i)
					if (!idx_next(&A.idx, &c, &r))
						break;
				if (i > A.sel + 1)
					A.sel++;
			} else {
				c = A.top;
				for (i = 0; i <= LIST_ROWS; ++i)
					if (!idx_next(&A.idx, &c, &r))
						break;
				if (i > LIST_ROWS) {
					c = A.top;
					idx_next(&A.idx, &c, &r);
					A.top = c;
				}
			}
		}
	} else if (key == K_UP || key == K_PGUP) {
		int steps = key == K_UP ? 1 : LIST_ROWS;
		while (steps--) {
			if (A.sel > 0)
				A.sel--;
			else
				idx_prev(&A.idx, &A.top);
		}
	} else if (key == K_ENTER) {
		c = A.top;
		for (i = 0; i <= A.sel; ++i)
			if (!idx_next(&A.idx, &c, &r))
				return;
		open_article(r.target, 0, 1);
	} else if (key == K_ESC) {
		if (A.query[0]) {
			A.query[0] = 0;
			A.qcur = 0;
			A.top_ok = 0;
		} else if (A.have_art) {
			A.screen = S_ARTICLE;
		} else if (A.nwikis > 1) {
			A.screen = S_WIKIS;
		}
	} else if (edit_key(A.query, sizeof(A.query), &A.qcur, key) == 1) {
		A.top_ok = 0;
	}
}

static void article_keys(int key, int mods)
{
	if (A.finding) {
		if (key == K_ESC) {
			A.finding = 0;
			A.hit = -1;
		} else if (key == K_ENTER || key == K_DOWN) {
			find_step(1);
		} else if (key == K_UP) {
			find_step(-1);
		} else if (edit_key(A.fquery, sizeof(A.fquery), &A.fcur, key) == 1) {
			A.searched = 0;
			A.hit = -1;
		}
		return;
	}
	switch (key) {
	case K_DOWN: case ' ':
		if (A.page + 1 < A.a.pages)
			load_page(A.page + 1);
		break;
	case K_UP:
		if (A.page > 0)
			load_page(A.page - 1);
		break;
	case K_PGDN:	/* Shift+Down on the PicoCalc */
		section_jump(1);
		break;
	case K_PGUP:	/* Shift+Up */
		section_jump(-1);
		break;
	case K_HOME:
		load_page(0);
		break;
	case K_END:
		load_page(A.a.pages ? A.a.pages - 1 : 0);
		break;
	case K_LEFT:
		if (mods & KM_SHIFT)
			section_jump(-1);
		else
			focus_move(-1);
		break;
	case K_RIGHT:
		if (mods & KM_SHIFT)
			section_jump(1);
		else
			focus_move(1);
		break;
	case K_TAB:
		focus_move((mods & KM_SHIFT) ? -1 : 1);
		break;
	case K_ENTER:
		follow();
		break;
	case K_ESC: case K_BS:
		go_back();
		break;
	case '[':
		section_jump(-1);
		break;
	case ']':
		section_jump(1);
		break;
	case 'l': case 'L':
		A.nsec = wiki_sections(&A.w, &A.a, A.sec, SEC_MAX);
		A.sec_sel = A.sec_top = 0;
		{
			int i;
			for (i = 0; i < A.nsec; ++i)
				if (A.sec[i].page <= A.page)
					A.sec_sel = i;
			if (A.sec_sel >= LIST_ROWS)
				A.sec_top = A.sec_sel - LIST_ROWS / 2;
		}
		A.screen = S_SECTIONS;
		break;
	case '/': case 'f': case 'F':
		A.finding = 1;
		break;
	case '*':
		{
			int i;
			for (i = 0; i < A.nfav; ++i)
				if (A.fav[i].art == A.art)
					break;
			if (i < A.nfav) {
				memmove(A.fav + i, A.fav + i + 1, (A.nfav - i - 1) * sizeof(list_ent_t));
				A.nfav--;
			} else {
				list_front(A.fav, &A.nfav, FAV_MAX, A.art, (uint16_t)A.page, A.title);
			}
			save_list("FAV", A.fav, A.nfav);
		}
		break;
	}
}

static void calc_run(void)
{
	/* the typed words become calc.c's letters */
	static const struct { const char *w; char c; } words[] = {
		{ "asin", 'S' }, { "acos", 'O' }, { "atan", 'T' }, { "sqrt", 'r' },
		{ "sin", 's' }, { "cos", 'o' }, { "tan", 't' }, { "log", 'g' },
		{ "ln", 'l' }, { "ans", 'A' }, { "pi", 'p' },
	};
	static char e[CALC_MAX * 2 + 1];
	size_t i = 0, n = 0, k;
	double v;
	int r;
	while (A.calc_in[i] && n < sizeof(e) - 1) {
		for (k = 0; k < NELEM(words); ++k) {
			size_t l = strlen(words[k].w), j;
			for (j = 0; j < l; ++j)
				if ((A.calc_in[i + j] | 0x20) != words[k].w[j])
					break;
			if (j == l)
				break;
		}
		if (k < NELEM(words)) {
			e[n++] = words[k].c;
			i += strlen(words[k].w);
		} else {
			char c = A.calc_in[i++];
			if (c == 'x' || c == 'X')
				c = '*';
			if (c != ' ')
				e[n++] = c;
		}
	}
	e[n] = 0;
	r = calc_eval(e, A.calc_ans, A.calc_rad ? 0 : CALC_DEG, &v);
	if (r == CALC_OK) {
		A.calc_ans = v;
		calc_format(v, A.calc_out);
		save_state();
	} else {
		strncpy(A.calc_out, calc_error_text(r), sizeof(A.calc_out) - 1);
	}
}

static void calc_keys(int key)
{
	if (key == K_ENTER || key == '=')
		calc_run();
	else if (key == K_TAB)
		A.calc_rad = !A.calc_rad;
	else if (key == K_ESC)
		A.screen = A.prev_screen;
	else if (edit_key(A.calc_in, sizeof(A.calc_in), &A.ccur, key) == 1 && !A.calc_in[0])
		A.calc_out[0] = 0;
}

static void random_article(void)
{
	static uint32_t seed;
	uint32_t n;
	int tries;
	if (!A.open)
		return;
	seed = seed * 1103515245u + 12345u + plat_ms();
	for (tries = 0; tries < 20; ++tries) {
		wiki_art_t a;
		seed = seed * 1103515245u + 12345u;
		n = (seed >> 8) % A.w.articles;
		/* prefer articles with more than a title */
		if (wiki_article(&A.w, n, &a) == 0 && a.pages >= 2)
			break;
	}
	open_article(n, 0, 1);
}

void app_key(int key, int mods)
{
	/* the lists always know the page you are on */
	if (A.screen == S_ARTICLE)
		sync_position();
	/* keys that work everywhere */
	if (key == K_F1) {
		/* pressed again on the search screen: clear the search; from
		 * elsewhere: back to the list as it was (scrolled, selected) */
		if (A.screen == S_SEARCH) {
			A.query[0] = 0;
			A.qcur = 0;
			A.top_ok = 0;
		}
		if (A.open && A.screen != S_SEARCH && on_article()) {
			A.screen = S_SEARCH;
			save_state();	/* switched off now: back to search */
		}
		A.screen = A.open ? S_SEARCH : (A.nwikis ? S_WIKIS : S_MSG);
		return;
	}
	if (key == K_F2 && A.open) {
		save_positions();
		if (A.screen != S_LISTS)
			A.prev_screen = A.screen;
		A.screen = S_LISTS;
		A.list_sel = A.list_top = 0;
		return;
	}
	if (key == K_F3) {
		random_article();
		return;
	}
	if (key == K_F4 && A.screen != S_CALC) {
		A.prev_screen = A.screen;
		A.screen = S_CALC;
		return;
	}
	if (key == K_F5 && A.screen != S_HELP) {
		A.prev_screen = A.screen;
		A.screen = S_HELP;
		return;
	}
	switch (A.screen) {
	case S_MSG:
		if (key == K_ESC || key == K_ENTER)
			A.screen = A.open ? (A.have_art ? S_ARTICLE : S_SEARCH) : S_WIKIS;
		break;
	case S_WIKIS:
		list_keys(key, A.nwikis, &A.wiki_sel, &(int){ 0 });
		if (key == K_ENTER && A.nwikis && A.open &&
		    strcmp(A.wikis[A.wiki_sel], A.name) == 0) {
			A.screen = S_SEARCH;	/* already open: its search */
			save_state();
		} else if (key == K_ENTER && A.nwikis && A.open) {
			/* another wiki: restart with it, so that it opens on a
			 * clean heap exactly as at power-on (a big wiki needs
			 * nearly all the memory, in large blocks) */
			save_positions();
			write_state(A.wikis[A.wiki_sel], 0);
			snprintf(A.msg, sizeof(A.msg), "Opening %s...", A.wiki_titles[A.wiki_sel]);
			A.screen = S_MSG;
			draw();
			plat_restart();
			/* no restart here (the PC): open it directly */
			if (open_wiki(A.wikis[A.wiki_sel]) == 0) {
				A.screen = S_SEARCH;
				save_state();
			}
		} else if (key == K_ENTER && A.nwikis && open_wiki(A.wikis[A.wiki_sel]) == 0) {
			A.screen = S_SEARCH;
			save_state();
		} else if (key == K_ESC && A.open) {
			A.screen = A.have_art ? S_ARTICLE : S_SEARCH;
		}
		break;
	case S_SEARCH:
		search_keys(key);
		break;
	case S_ARTICLE:
		article_keys(key, mods);
		break;
	case S_SECTIONS:
		list_keys(key, A.nsec, &A.sec_sel, &A.sec_top);
		if (key == K_ENTER && A.nsec) {
			load_page(A.sec[A.sec_sel].page);
			A.screen = S_ARTICLE;
		} else if (key == K_ESC || key == 'l' || key == 'L') {
			A.screen = S_ARTICLE;
		}
		break;
	case S_LISTS:
		if (key == K_TAB || key == K_LEFT || key == K_RIGHT) {
			A.list_tab = !A.list_tab;
			A.list_sel = A.list_top = 0;
			break;
		}
		list_keys(key, A.list_tab ? A.nfav : A.nhist, &A.list_sel, &A.list_top);
		if (key == K_ENTER) {
			list_ent_t e;
			if (A.list_tab ? A.list_sel < A.nfav : A.list_sel < A.nhist) {
				e = A.list_tab ? A.fav[A.list_sel] : A.hist[A.list_sel];
				open_entry(&e);
			}
		} else if (key == K_DEL) {
			list_ent_t *l = A.list_tab ? A.fav : A.hist;
			int *n = A.list_tab ? &A.nfav : &A.nhist;
			if (A.list_sel < *n) {
				memmove(l + A.list_sel, l + A.list_sel + 1,
					(*n - A.list_sel - 1) * sizeof(list_ent_t));
				(*n)--;
				save_list(A.list_tab ? "FAV" : "HIS", l, *n);
				if (A.list_sel >= *n && A.list_sel)
					A.list_sel--;
			}
		} else if (key == K_ESC) {
			A.screen = A.have_art ? S_ARTICLE : S_SEARCH;
		}
		break;
	case S_CALC:
		calc_keys(key);
		break;
	case S_HELP:
		if (key == K_ESC || key == K_F5 || key == K_ENTER)
			A.screen = A.prev_screen;
		else if ((key == 'w' || key == 'W') && A.nwikis) {
			A.screen = S_WIKIS;
		}
		break;
	}
}

/* ------------------------------------------------------------------ */

void app_init(void)
{
	uint8_t *st = NULL;
	uint32_t len = 0;
	int safe = plat_keys_held(), i;
	memset(&A, 0, sizeof(A));
	A.screen = S_WIKIS;
	A.hint_on = 1;
	A.hint_t0 = plat_ms();
	A.focus = -1;
	A.hit = -1;
	find_wikis();
	if (plat_load(STATE_FILE, &st, &len) == 0 && len >= 48 && zw_rd32(st) == STATE_MAGIC && !safe) {
		char name[16];
		memcpy(&A.calc_ans, st + 32, sizeof(double));
		A.calc_rad = st[40] != 0;
		memcpy(name, st + 4, 16);
		name[15] = 0;
		for (i = 0; i < A.nwikis; ++i)
			if (strcmp(A.wikis[i], name) == 0 && open_wiki(name) == 0) {
				A.wiki_sel = i;
				A.screen = S_SEARCH;
				if (st[26] && zw_rd32(st + 20) < A.w.articles) {
					open_article(zw_rd32(st + 20), zw_rd16(st + 24), 0);
					if (st[27] && A.have_art)
						A.screen = S_SEARCH;	/* was searching */
				}
				if (len >= 114 && st[48]) {
					memcpy(A.query, st + 48, QUERY_MAX);
					A.query[QUERY_MAX] = 0;
					A.qcur = strlen(A.query);
					A.top_ok = 0;
					A.sel = zw_rd16(st + 112);
					if (A.sel >= LIST_ROWS)
						A.sel = 0;
				}
				break;
			}
	}
	zw_free(st);
	if (!A.open) {
		if (A.nwikis == 1 && open_wiki(A.wikis[0]) == 0)
			A.screen = S_SEARCH;
		else if (A.screen != S_MSG && !A.nwikis)
			A.screen = S_WIKIS;
	}
	draw();
}

void app_redraw(void)
{
	draw();
}

void app_event(const plat_event_t *e)
{
	if (e->type == PE_KEY) {
		A.hint_on = 0;
		app_key(e->key, e->mods);
		draw();
	} else if (e->type == PE_TICK) {
		if (A.hint_on && plat_ms() - A.hint_t0 >= 10000) {
			A.hint_on = 0;
			draw();
		}
	} else if (e->type == PE_IDLE) {
		if (A.open) {
			save_state();
			save_positions();
		}
	}
}

int app_run(void)
{
	plat_event_t e;
	app_init();
	for (;;) {
		if (!plat_wait(&e))
			continue;
		if (e.type == PE_QUIT)
			break;
		app_event(&e);
	}
	if (A.have_art)
		save_state();
	close_wiki();
	return 0;
}

/* for tests: a one-line description of the screen */
size_t app_state_size(void) { return sizeof(A); }

void app_describe(char *out, size_t cap)
{
	switch (A.screen) {
	case S_ARTICLE:
		snprintf(out, cap, "article \"%s\" page=%u/%u focus=%d links=%u back=%d%s%s",
			 A.title, A.page + 1, A.a.pages, A.focus, A.pg.nlinks, A.nback,
			 is_fav(A.art) ? " fav" : "", A.finding ? " finding" : "");
		if (A.finding && A.searched) {
			size_t n = strlen(out);
			snprintf(out + n, cap - n, " hits=%d hit=%d", A.nhits, A.hit + 1);
		}
		break;
	case S_SEARCH: {
		/* the selected title too, to check the list position */
		idx_cursor_t c = A.top;
		idx_record_t r;
		int i;
		r.title[0] = 0;
		for (i = 0; A.open && A.top_ok && i <= A.sel; ++i)
			if (!idx_next(&A.idx, &c, &r)) {
				r.title[0] = 0;
				break;
			}
		snprintf(out, cap, "search \"%s\" sel=%d \"%s\"", A.query, A.sel, r.title);
		break;
	}
	case S_SECTIONS:
		snprintf(out, cap, "sections n=%d sel=%d", A.nsec, A.sec_sel);
		break;
	case S_LISTS:
		snprintf(out, cap, "%s n=%d sel=%d", A.list_tab ? "favourites" : "history",
			 A.list_tab ? A.nfav : A.nhist, A.list_sel);
		break;
	case S_CALC:
		snprintf(out, cap, "calc \"%s\" = \"%s\"", A.calc_in, A.calc_out);
		break;
	case S_HELP:
		snprintf(out, cap, "help");
		break;
	case S_WIKIS:
		snprintf(out, cap, "wikis n=%d sel=%d", A.nwikis, A.wiki_sel);
		break;
	default:
		snprintf(out, cap, "message \"%s\"", A.msg);
	}
}
