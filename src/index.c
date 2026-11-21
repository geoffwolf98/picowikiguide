/*
 * zimwiki - title search index reader (see index.h)
 */
#include "index.h"
#include "fold.h"

int idx_keycmp(const char *a, size_t alen, const char *b, size_t blen)
{
	size_t n = alen < blen ? alen : blen;
	int c = memcmp(a, b, n);
	if (c)
		return c;
	return alen < blen ? -1 : alen > blen ? 1 : 0;
}

int idx_parse_header(idx_t *x, const uint8_t *h)
{
	if (zw_rd32(h + IDX_H_MAGIC) != IDX_MAGIC)
		return ZW_EFORMAT;
	if (zw_rd32(h + IDX_H_VERSION) != IDX_VERSION)
		return ZW_EUNSUPPORTED;
	x->pages = zw_rd32(h + IDX_H_PAGES);
	x->records = zw_rd32(h + IDX_H_RECORDS);
	x->articles = zw_rd32(h + IDX_H_ARTICLES);
	x->top_off = zw_rd32(h + IDX_H_TOP_OFF);
	x->sum_off = zw_rd32(h + IDX_H_SUM_OFF);
	x->sum_count = zw_rd32(h + IDX_H_SUM_COUNT);
	memcpy(x->uuid, h + IDX_H_UUID, 16);
	x->zim_entries = zw_rd32(h + IDX_H_ZIM_ENTRIES);
	x->chunks = zw_rd32(h + IDX_H_CHUNKS);
	x->chunk_shift = h[IDX_H_CHUNK_SHIFT];
	x->zim_size = zw_rd64(h + IDX_H_ZIM_SIZE);
	x->main_entry = zw_rd32(h + IDX_H_MAIN);
	memcpy(x->title, h + IDX_H_TITLE, IDX_H_TITLE_LEN);
	x->title[IDX_H_TITLE_LEN - 1] = 0;
	memcpy(x->lang, h + IDX_H_LANG, IDX_H_LANG_LEN);
	x->lang[IDX_H_LANG_LEN - 1] = 0;
	memcpy(x->name, h + IDX_H_NAME, IDX_H_NAME_LEN);
	x->name[IDX_H_NAME_LEN - 1] = 0;
	if (x->chunk_shift < 20 || x->chunk_shift > 31 || !x->chunks || x->chunks > 100)
		return ZW_EFORMAT;
	if (x->sum_count != (x->pages + IDX_STRIDE - 1) / IDX_STRIDE)
		return ZW_EFORMAT;
	return ZW_OK;
}

static uint32_t idx_step = 4 * IDX_STRIDE;

int idx_open(idx_t *x, const zw_io_t *io)
{
	uint8_t h[512];
	uint32_t i;
	int r;

	memset(x, 0, sizeof(*x));
	x->io = *io;
	if ((r = zw_io_read(io, 0, h, sizeof(h))) < 0)
		return r;
	if ((r = idx_parse_header(x, h)) < 0)
		return r;
	if ((uint64_t)x->top_off + (uint64_t)x->pages * IDX_TOP > io->size ||
	    (uint64_t)x->sum_off + (uint64_t)x->sum_count * IDX_TOP > io->size)
		return ZW_EFORMAT;
	x->step = idx_step;
	x->ram_count = (x->pages + x->step - 1) / x->step;
	x->summary = zw_alloc(x->ram_count * IDX_TOP + 1);
	if (!x->summary)
		return ZW_ENOMEM;
	for (i = 0; i < x->ram_count; ++i) {
		uint64_t at = x->step % IDX_STRIDE == 0 ?
			x->sum_off + (uint64_t)i * (x->step / IDX_STRIDE) * IDX_TOP :
			x->top_off + (uint64_t)i * x->step * IDX_TOP;
		if ((r = zw_io_read(io, at, x->summary + i * IDX_TOP, IDX_TOP)) < 0) {
			idx_close(x);
			return r;
		}
	}
	return ZW_OK;
}

void idx_set_step(uint32_t step)
{
	idx_step = step ? step : 1;
}

void idx_close(idx_t *x)
{
	zw_free(x->summary);
	x->summary = NULL;
}

static int load_page(idx_t *x, uint32_t page)
{
	int r;
	if (x->page_no == page)
		return ZW_OK;
	if (page < 1 || page > x->pages)
		return ZW_ERANGE;
	x->page_no = 0;
	if ((r = zw_io_read(&x->io, (uint64_t)page * IDX_PAGE, x->page, IDX_PAGE)) < 0)
		return r;
	x->page_no = page;
	return ZW_OK;
}

/* top entry key < query (query already truncated to IDX_KEY) */
static int top_less(const uint8_t *e, const char *q, size_t qlen)
{
	uint32_t klen = e[4] > IDX_KEY ? IDX_KEY : e[4];
	return idx_keycmp((const char *)e + 5, klen, q, qlen) < 0;
}

/* record at page offset: returns header pointer or NULL at end of page */
static const uint8_t *rec_at(idx_t *x, uint32_t off)
{
	const uint8_t *p = x->page + off;
	if (off + IDX_REC_HDR > IDX_PAGE || p[0] == 0 ||
	    off + IDX_REC_HDR + p[0] > IDX_PAGE)
		return NULL;
	return p;
}

/* top entry i (0 based page) from the card */
static int top_entry(idx_t *x, uint32_t i, uint8_t *e)
{
	return zw_io_read(&x->io, x->top_off + (uint64_t)i * IDX_TOP, e, IDX_TOP);
}

/* find the page whose range may hold the first key >= q, using the
 * summary in RAM and then the top entries of one span on the card */
static int find_page(idx_t *x, const char *q, size_t qlen, uint32_t *page,
		     uint32_t *first_record)
{
	uint8_t e[IDX_TOP];
	uint32_t lo = 0, hi = x->ram_count, seg, n, t;
	size_t qt = qlen > IDX_KEY ? IDX_KEY : qlen;
	int r;

	while (lo < hi) {
		uint32_t mid = (lo + hi) / 2;
		if (top_less(x->summary + mid * IDX_TOP, q, qt))
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo == 0) {
		*page = 1;
		*first_record = 0;
		return ZW_OK;
	}
	seg = (lo - 1) * x->step;
	n = x->pages - seg;
	if (n > x->step + 1)
		n = x->step + 1;
	/* entry 0 of the span is known to be < q */
	lo = 1;
	hi = n;
	while (lo < hi) {
		uint32_t mid = (lo + hi) / 2;
		if ((r = top_entry(x, seg + mid, e)) < 0)
			return r;
		if (top_less(e, q, qt))
			lo = mid + 1;
		else
			hi = mid;
	}
	t = lo - 1;
	if ((r = top_entry(x, seg + t, e)) < 0)
		return r;
	*page = seg + t + 1;
	*first_record = zw_rd32(e);
	return ZW_OK;
}

static void rec_fill(const uint8_t *p, idx_record_t *r)
{
	memcpy(r->title, p + IDX_REC_HDR, p[0]);
	r->title[p[0]] = 0;
	r->flags = p[1];
	r->target = zw_rd32(p + 2);
}

int idx_seek(idx_t *x, const char *q, idx_cursor_t *c)
{
	size_t qlen = strlen(q);
	uint32_t page, num, off;
	char key[256];
	int r;

	if (!x->pages) {
		c->page = 1;
		c->off = 0;
		c->number = 0;
		return ZW_OK;
	}
	if ((r = find_page(x, q, qlen, &page, &num)) < 0)
		return r;
	for (; page <= x->pages; ++page) {
		if ((r = load_page(x, page)) < 0)
			return r;
		off = 0;
		for (;;) {
			const uint8_t *p = rec_at(x, off);
			size_t klen;
			if (!p)
				break;
			klen = fold_utf8((const char *)p + IDX_REC_HDR, p[0], key, sizeof(key));
			if (idx_keycmp(key, klen, q, qlen) >= 0) {
				c->page = page;
				c->off = (uint16_t)off;
				c->number = num;
				return ZW_OK;
			}
			off += IDX_REC_HDR + p[0];
			num++;
		}
	}
	/* past the end */
	c->page = x->pages + 1;
	c->off = 0;
	c->number = x->records;
	return ZW_OK;
}

int idx_seek_number(idx_t *x, uint32_t n, idx_cursor_t *c)
{
	uint8_t e[IDX_TOP];
	uint32_t lo = 0, hi = x->ram_count, seg, cnt, t, num, off;
	int r;

	if (n >= x->records)
		return ZW_ERANGE;
	while (lo < hi) {
		uint32_t mid = (lo + hi) / 2;
		if (zw_rd32(x->summary + mid * IDX_TOP) <= n)
			lo = mid + 1;
		else
			hi = mid;
	}
	seg = lo ? (lo - 1) * x->step : 0;
	cnt = x->pages - seg;
	if (cnt > x->step)
		cnt = x->step;
	lo = 1;
	hi = cnt;
	while (lo < hi) {
		uint32_t mid = (lo + hi) / 2;
		if ((r = top_entry(x, seg + mid, e)) < 0)
			return r;
		if (zw_rd32(e) <= n)
			lo = mid + 1;
		else
			hi = mid;
	}
	t = lo - 1;
	if ((r = top_entry(x, seg + t, e)) < 0)
		return r;
	c->page = seg + t + 1;
	num = zw_rd32(e);
	if ((r = load_page(x, c->page)) < 0)
		return r;
	off = 0;
	while (num < n) {
		const uint8_t *p = rec_at(x, off);
		if (!p)
			return ZW_EFORMAT;
		off += IDX_REC_HDR + p[0];
		num++;
	}
	c->off = (uint16_t)off;
	c->number = num;
	return ZW_OK;
}

int idx_next(idx_t *x, idx_cursor_t *c, idx_record_t *r)
{
	const uint8_t *p;
	int e;

	for (;;) {
		if (c->page > x->pages || c->page == 0)
			return 0;
		if ((e = load_page(x, c->page)) < 0)
			return e;
		p = rec_at(x, c->off);
		if (p)
			break;
		c->page++;
		c->off = 0;
	}
	rec_fill(p, r);
	r->number = c->number;
	c->off = (uint16_t)(c->off + IDX_REC_HDR + p[0]);
	c->number++;
	/* keep one canonical form: never leave the cursor at the end of a page */
	if (!rec_at(x, c->off)) {
		c->page++;
		c->off = 0;
	}
	return 1;
}

int idx_prev(idx_t *x, idx_cursor_t *c)
{
	uint32_t page = c->page, off, last = 0xffffffffu;
	int e;

	if (c->number == 0)
		return 0;
	if (page > x->pages) {
		page = x->pages;
		off = IDX_PAGE;
	} else {
		off = c->off;
	}
	for (;;) {
		uint32_t o = 0;
		if ((e = load_page(x, page)) < 0)
			return e;
		for (;;) {
			const uint8_t *p = rec_at(x, o);
			if (!p || o >= off)
				break;
			last = o;
			o += IDX_REC_HDR + p[0];
		}
		if (last != 0xffffffffu) {
			c->page = page;
			c->off = (uint16_t)last;
			c->number--;
			return 1;
		}
		if (page <= 1)
			return 0;
		page--;
		off = IDX_PAGE;
	}
}
