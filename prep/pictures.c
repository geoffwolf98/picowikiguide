/*
 * PicoWikiGuide zimprep --pictures (after Gen2's tools/pictures.c)
 *
 * 1. every article's HTML is scanned for <img> tags; each src is turned
 *    into the ZIM entry it names (pictures only: not maths, icons or
 *    SVG); the first of each article (lead) or all (up to PIC_MAX)
 * 2. each picture is converted once, however many articles show it:
 *    decoded (WebP with the vendored libwebp, PNG/JPEG/GIF with
 *    stb_image), scaled to fit (PWG_PIC_MAX_W x PWG_PIC_MAX_H, never
 *    enlarged), made grey, contrast stretched, sharpened a little, and
 *    dithered (Floyd-Steinberg, in linear light) to the screen's four
 *    greys; the CPU work runs on all cores
 * 3. written to NAME.I00 ... with a table (DESIGN.md 4.5); the layout
 *    then asks pictures_for() per article
 */
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <stdint.h>
#include "pictures.h"
#include "hostio.h"
#include "card.h"
#include "zstd.h"
#include "src/webp/decode.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_PSD
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_TGA
#define STBI_MAX_DIMENSIONS 16384
#include "stb_image.h"

#define MIN_ATTR   40		/* width/height attributes below: an icon */
#define MIN_SIDE   24		/* converted pictures smaller than this: dropped */
#define BATCH_IMGS 256
#define BATCH_MB   96

/* the screen's greys, as written to the panel (0 white ... 3 black) */
static const int lcd_level[4] = { 0xfc, 0x9c, 0x4c, 0x00 };

static void progress(const char *what, uint64_t done, uint64_t total)
{
	static int last = -1;
	int pc = total ? (int)(done * 100 / total) : 100;
	if (pc != last || done == total) {
		fprintf(stderr, "\r%s %3d%%", what, pc);
		if (done == total)
			fprintf(stderr, "\n");
		last = done == total ? -1 : pc;
	}
}

/* ------------------------------------------------------------------ */
/* step 1: the pictures of each article */

typedef struct img {
	uint32_t entry;		/* the picture's ZIM entry */
	uint16_t w, h;		/* converted size, 0: failed */
	uint32_t number;	/* in the table */
} img_t;

typedef struct art {
	uint32_t entry;		/* the article's ZIM entry */
	uint32_t first;		/* into refs */
	uint8_t n;
} art_t;

typedef struct ref {
	uint32_t hash;
	uint32_t img;		/* into imgs */
} ref_t;

static img_t *imgs;
static uint32_t nimgs, imgs_cap;
static uint32_t *img_tab;	/* open addressing: picture entry -> imgs index + 1 */
static uint32_t img_tab_mask;
static art_t *arts;
static uint32_t narts, arts_cap;
static ref_t *refs;
static uint32_t nrefs, refs_cap;
static uint32_t nconverted;

static void *grow(void *p, uint32_t *cap, uint32_t need, size_t elem)
{
	if (need <= *cap)
		return p;
	*cap = *cap ? *cap * 2 : 1024;
	while (*cap < need)
		*cap *= 2;
	p = realloc(p, (size_t)*cap * elem);
	if (!p) {
		fprintf(stderr, "zimprep: out of memory\n");
		exit(1);
	}
	return p;
}

static uint32_t img_index(uint32_t entry)
{
	uint32_t h = (entry * 2654435761u) & img_tab_mask;
	while (img_tab[h]) {
		if (imgs[img_tab[h] - 1].entry == entry)
			return img_tab[h] - 1;
		h = (h + 1) & img_tab_mask;
	}
	if (nimgs * 2 >= img_tab_mask) {
		uint32_t i, old = img_tab_mask + 1;
		uint32_t *t = img_tab;
		img_tab_mask = old * 2 - 1;
		img_tab = calloc(old * 2, sizeof(uint32_t));
		for (i = 0; i < old; ++i)
			if (t[i]) {
				uint32_t k = (imgs[t[i] - 1].entry * 2654435761u) & img_tab_mask;
				while (img_tab[k])
					k = (k + 1) & img_tab_mask;
				img_tab[k] = t[i];
			}
		free(t);
		return img_index(entry);
	}
	imgs = grow(imgs, &imgs_cap, nimgs + 1, sizeof(img_t));
	memset(&imgs[nimgs], 0, sizeof(img_t));
	imgs[nimgs].entry = entry;
	img_tab[h] = ++nimgs;
	return nimgs - 1;
}

static int hexv(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	c |= 0x20;
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	return -1;
}

/* attribute value of name in the tag text [s, e), or NULL */
static const char *attr(const char *s, const char *e, const char *name, uint32_t *len)
{
	size_t nl = strlen(name);
	while (s < e) {
		const char *n0, *v = NULL;
		uint32_t vl = 0;
		size_t k;
		while (s < e && (*s == ' ' || *s == '\n' || *s == '\t' || *s == '\r' || *s == '/'))
			s++;
		n0 = s;
		while (s < e && *s != '=' && *s != ' ' && *s != '>' && *s != '\n' && *s != '\t' && *s != '/')
			s++;
		if (s == n0) {
			s++;
			continue;
		}
		k = (size_t)(s - n0);
		while (s < e && (*s == ' ' || *s == '\n'))
			s++;
		if (s < e && *s == '=') {
			s++;
			while (s < e && (*s == ' ' || *s == '\n'))
				s++;
			if (s < e && (*s == '"' || *s == '\'')) {
				char q = *s++;
				v = s;
				while (s < e && *s != q)
					s++;
				vl = (uint32_t)(s - v);
				if (s < e)
					s++;
			} else {
				v = s;
				while (s < e && *s != ' ' && *s != '>' && *s != '\n')
					s++;
				vl = (uint32_t)(s - v);
			}
		}
		if (k == nl && strncasecmp(n0, name, nl) == 0) {
			*len = vl;
			return v ? v : "";
		}
	}
	return NULL;
}

static int has_word(const char *s, uint32_t n, const char *w)
{
	size_t k = strlen(w);
	uint32_t i;
	for (i = 0; i + k <= n; ++i)
		if (strncasecmp(s + i, w, k) == 0)
			return 1;
	return 0;
}

/* the entry a src names, relative to the article at (ns, url); -1 if
 * none or not a picture we can use */
static int resolve(zim_t *z, char ns, const char *url, const char *src, uint32_t slen,
		   uint32_t *entry)
{
	char path[1024], dec[1024];
	char *seg[128];
	int nseg = 0, i;
	uint32_t k, o = 0, idx, fin;
	zim_dirent_t d;
	char *p, *q;

	if (!slen || slen >= 512 || !memcmp(src, "data:", 5) || !memcmp(src, "http:", 5) ||
	    !memcmp(src, "https:", 6) || (slen > 1 && src[0] == '/' && src[1] == '/'))
		return -1;
	for (k = 0; k < slen && o < sizeof(dec) - 1; ++k) {
		if (src[k] == '&' && slen - k >= 5 && !memcmp(src + k, "&amp;", 5)) {
			dec[o++] = '&';
			k += 4;
		} else if (src[k] == '%' && k + 2 < slen && hexv(src[k + 1]) >= 0 && hexv(src[k + 2]) >= 0) {
			dec[o++] = (char)(hexv(src[k + 1]) * 16 + hexv(src[k + 2]));
			k += 2;
		} else if (src[k] == '?' || src[k] == '#') {
			break;
		} else {
			dec[o++] = src[k];
		}
	}
	dec[o] = 0;
	snprintf(path, sizeof(path), "%c/%s", ns, url);
	if ((p = strrchr(path, '/')))
		p[1] = 0;
	if (strlen(path) + strlen(dec) + 1 >= sizeof(path))
		return -1;
	strcat(path, dec);
	for (p = path; p && *p; p = q) {
		q = strchr(p, '/');
		if (q)
			*q++ = 0;
		if (!strcmp(p, ".") || !*p)
			continue;
		if (!strcmp(p, "..")) {
			if (nseg > 0)
				nseg--;
			continue;
		}
		if (nseg < 128)
			seg[nseg++] = p;
	}
	if (nseg < 2 || strlen(seg[0]) != 1)
		return -1;
	dec[0] = 0;
	for (i = 1; i < nseg; ++i) {
		if (i > 1)
			strcat(dec, "/");
		strcat(dec, seg[i]);
	}
	if (zim_find(z, seg[0][0], dec, &idx) < 0)
		return -1;
	if (zim_follow(z, idx, &fin, &d) < 0)
		return -1;
	if (d.mime == ZIM_MIME_REDIRECT || d.mime == ZIM_MIME_LINKTARGET || d.mime == ZIM_MIME_DELETED ||
	    d.mime >= z->nmime || strncmp(z->mime[d.mime], "image/", 6) != 0 ||
	    strstr(z->mime[d.mime], "svg"))
		return -1;
	*entry = fin;
	return 0;
}

/* the pictures of one article: their src hashes and picture entries */
static int scan_one(zim_t *z, const zim_dirent_t *ad, const char *h, uint32_t n, int mode,
		    uint32_t *hashes, uint32_t *pes)
{
	const char *p = h, *end = h + n;
	int count = 0;
	while (p < end && count < (mode == PICS_LEAD ? 1 : PIC_MAX)) {
		const char *t = memchr(p, '<', (size_t)(end - p)), *te;
		const char *src, *v;
		uint32_t slen = 0, vl = 0, pe, hash;
		int i, dup = 0;
		if (!t)
			break;
		p = t + 1;
		if (end - p > 8 && !strncasecmp(p, "noscript", 8)) {
			const char *c = p;
			while (c && c < end) {
				c = memchr(c, '<', (size_t)(end - c));
				if (!c || (end - c > 11 && !strncasecmp(c, "</noscript", 10)))
					break;
				c++;
			}
			p = c ? c + 1 : end;
			continue;
		}
		if (end - p < 4 || strncasecmp(p, "img", 3) || (p[3] != ' ' && p[3] != '\n' && p[3] != '\t'))
			continue;
		te = memchr(p, '>', (size_t)(end - p));
		if (!te)
			break;
		p += 3;
		if ((v = attr(p, te, "class", &vl)) && has_word(v, vl, "mwe-math"))
			continue;
		if ((v = attr(p, te, "width", &vl)) && vl && atoi(v) > 0 && atoi(v) < MIN_ATTR)
			continue;
		if ((v = attr(p, te, "height", &vl)) && vl && atoi(v) > 0 && atoi(v) < MIN_ATTR)
			continue;
		src = attr(p, te, "src", &slen);
		if (!src || !slen || !memcmp(src, "data:", 5))
			src = attr(p, te, "data-src", &slen);
		if (!src || !slen)
			continue;
		if (resolve(z, ad->ns, ad->url, src, slen, &pe) < 0)
			continue;
		hash = pic_src_hash(src, slen);
		for (i = 0; i < count; ++i)
			if (hashes[i] == hash)
				dup = 1;
		if (dup)
			continue;
		pes[count] = pe;
		hashes[count++] = hash;
	}
	return count;
}

static void add_article(uint32_t entry, int count, const uint32_t *hashes, const uint32_t *pes)
{
	uint32_t first = nrefs;
	int k;
	if (!count)
		return;
	for (k = 0; k < count; ++k) {
		refs = grow(refs, &refs_cap, nrefs + 1, sizeof(ref_t));
		refs[nrefs].hash = hashes[k];
		refs[nrefs].img = img_index(pes[k]);
		nrefs++;
	}
	arts = grow(arts, &arts_cap, narts + 1, sizeof(art_t));
	arts[narts].entry = entry;
	arts[narts].first = first;
	arts[narts].n = (uint8_t)count;
	narts++;
}

#define SCAN_BLOCK 16384

typedef struct sres {
	uint32_t n;
	uint32_t *entry;
	uint8_t *count;
	uint32_t *hash, *pe;
	int done;
} sres_t;

typedef struct scan {
	const char *input;
	int mode;
	uint32_t entries, nblocks, next;
	sres_t *res;
	pthread_mutex_t m;
	pthread_cond_t done;
} scan_t;

typedef struct ekey {
	uint32_t cluster, blob, entry;
} ekey_t;

static int ekey_cmp(const void *a, const void *b)
{
	const ekey_t *x = a, *y = b;
	if (x->cluster != y->cluster)
		return x->cluster < y->cluster ? -1 : 1;
	if (x->blob != y->blob)
		return x->blob < y->blob ? -1 : 1;
	return x->entry < y->entry ? -1 : x->entry > y->entry;
}

typedef struct hit {
	uint32_t entry, at;
	uint8_t count;
} hit_t;

static int hit_cmp(const void *a, const void *b)
{
	const hit_t *x = a, *y = b;
	return x->entry < y->entry ? -1 : x->entry > y->entry;
}

static void *scan_worker(void *arg)
{
	scan_t *sc = arg;
	host_files_t f;
	zim_t z;
	ekey_t *keys = malloc(sizeof(ekey_t) * SCAN_BLOCK);
	hit_t *hits = malloc(sizeof(hit_t) * SCAN_BLOCK);
	uint32_t *hash = NULL, *pe = NULL, hcap = 0;
	if (host_files_open(&f, sc->input) < 0 || zim_open(&z, &f.io) < 0) {
		fprintf(stderr, "zimprep: cannot open %s again\n", sc->input);
		exit(1);
	}
	for (;;) {
		uint32_t b, first, last, i, nk = 0, nh = 0, np = 0;
		sres_t *r;
		pthread_mutex_lock(&sc->m);
		b = sc->next < sc->nblocks ? sc->next++ : UINT32_MAX;
		pthread_mutex_unlock(&sc->m);
		if (b == UINT32_MAX)
			break;
		first = b * SCAN_BLOCK;
		last = first + SCAN_BLOCK < sc->entries ? first + SCAN_BLOCK : sc->entries;
		for (i = first; i < last; ++i) {
			zim_dirent_t d;
			if (zim_dirent(&z, i, &d) < 0 || d.mime == ZIM_MIME_REDIRECT ||
			    d.mime == ZIM_MIME_LINKTARGET || d.mime == ZIM_MIME_DELETED ||
			    d.mime >= z.nmime || strncmp(z.mime[d.mime], "text/html", 9) != 0)
				continue;
			keys[nk].cluster = d.cluster;
			keys[nk].blob = d.blob;
			keys[nk].entry = i;
			nk++;
		}
		qsort(keys, nk, sizeof(ekey_t), ekey_cmp);
		for (i = 0; i < nk; ++i) {
			zim_dirent_t d;
			const uint8_t *data;
			uint32_t len, hh[PIC_MAX], pp[PIC_MAX];
			int c, k;
			char *h;
			if (zim_dirent(&z, keys[i].entry, &d) < 0 ||
			    zim_blob(&z, d.cluster, d.blob, &data, &len) < 0)
				continue;
			h = malloc(len ? len : 1);	/* resolving src reads the ZIM again */
			memcpy(h, data, len);
			c = scan_one(&z, &d, h, len, sc->mode, hh, pp);
			free(h);
			if (!c)
				continue;
			if (np + (uint32_t)c > hcap) {
				hcap = hcap ? hcap * 2 : 1024;
				while (hcap < np + (uint32_t)c)
					hcap *= 2;
				hash = realloc(hash, sizeof(uint32_t) * hcap);
				pe = realloc(pe, sizeof(uint32_t) * hcap);
			}
			hits[nh].entry = keys[i].entry;
			hits[nh].at = np;
			hits[nh].count = (uint8_t)c;
			nh++;
			for (k = 0; k < c; ++k) {
				hash[np + (uint32_t)k] = hh[k];
				pe[np + (uint32_t)k] = pp[k];
			}
			np += (uint32_t)c;
		}
		qsort(hits, nh, sizeof(hit_t), hit_cmp);
		r = &sc->res[b];
		r->n = nh;
		r->entry = malloc(sizeof(uint32_t) * (nh ? nh : 1));
		r->count = malloc(nh ? nh : 1);
		r->hash = malloc(sizeof(uint32_t) * (np ? np : 1));
		r->pe = malloc(sizeof(uint32_t) * (np ? np : 1));
		{
			uint32_t at = 0;
			for (i = 0; i < nh; ++i) {
				r->entry[i] = hits[i].entry;
				r->count[i] = hits[i].count;
				memcpy(r->hash + at, hash + hits[i].at, 4u * hits[i].count);
				memcpy(r->pe + at, pe + hits[i].at, 4u * hits[i].count);
				at += hits[i].count;
			}
		}
		pthread_mutex_lock(&sc->m);
		r->done = 1;
		pthread_cond_broadcast(&sc->done);
		pthread_mutex_unlock(&sc->m);
	}
	zim_close(&z);
	host_files_close(&f);
	free(keys);
	free(hits);
	free(hash);
	free(pe);
	return NULL;
}

/* ------------------------------------------------------------------ */
/* step 2: conversion */

typedef struct job {
	uint32_t img;
	uint8_t *raw;
	uint32_t raw_len;
	uint8_t *rec;		/* the record (header + data), or NULL */
	uint32_t rec_len;
	uint16_t w, h;
} job_t;

static void fit(int sw, int sh, int *tw, int *th)
{
	double s = 1.0;
	if (sw > PWG_PIC_MAX_W)
		s = (double)PWG_PIC_MAX_W / sw;
	if (sh * s > PWG_PIC_MAX_H)
		s = (double)PWG_PIC_MAX_H / sh;
	*tw = (int)(sw * s + 0.5);
	*th = (int)(sh * s + 0.5);
	if (*tw < 1)
		*tw = 1;
	if (*th < 1)
		*th = 1;
	if (*tw > PWG_PIC_MAX_W)
		*tw = PWG_PIC_MAX_W;
	if (*th > PWG_PIC_MAX_H)
		*th = PWG_PIC_MAX_H;
}

/* RGBA (any size) -> grey (sRGB codes) at tw x th, white behind
 * transparency, by area averaging */
static uint8_t *to_grey(const uint8_t *rgba, int sw, int sh, int tw, int th)
{
	uint8_t *g = malloc((size_t)tw * th);
	int x, y;
	for (y = 0; y < th; ++y) {
		int y0 = y * sh / th, y1 = (y + 1) * sh / th;
		if (y1 <= y0)
			y1 = y0 + 1;
		for (x = 0; x < tw; ++x) {
			int x0 = x * sw / tw, x1 = (x + 1) * sw / tw, i, j;
			uint32_t sum = 0, cnt = 0;
			if (x1 <= x0)
				x1 = x0 + 1;
			for (j = y0; j < y1; ++j)
				for (i = x0; i < x1; ++i) {
					const uint8_t *q = rgba + ((size_t)j * sw + i) * 4;
					uint32_t l = (77u * q[0] + 150u * q[1] + 29u * q[2]) >> 8;
					sum += (l * q[3] + 255u * (255u - q[3])) / 255u;
					cnt++;
				}
			g[(size_t)y * tw + x] = (uint8_t)(sum / cnt);
		}
	}
	return g;
}

static float lin(int code)
{
	return powf(code / 255.0f, 2.2f);
}

/* contrast stretch (1% cut at each end), light sharpening, then
 * Floyd-Steinberg (serpentine) in linear light to the four screen greys:
 * 2 bits a pixel, 0 white ... 3 black, 4 pixels a byte, first in the top
 * bits, rows padded to whole bytes */
static uint8_t *dither4(const uint8_t *g, int w, int h)
{
	uint32_t hist[256] = { 0 }, acc = 0, total = (uint32_t)(w * h);
	int lo, hi, x, y, i;
	float *e = malloc(sizeof(float) * (size_t)w * h), pal[4];
	int stride = (w * 2 + 7) >> 3;
	uint8_t *bits = calloc((size_t)stride * h, 1);
	for (i = 0; i < 4; ++i)
		pal[i] = lin(lcd_level[i]);
	for (i = 0; i < w * h; ++i)
		hist[g[i]]++;
	for (lo = 0; lo < 255 && (acc += hist[lo]) < total / 100; ++lo)
		;
	acc = 0;
	for (hi = 255; hi > 0 && (acc += hist[hi]) < total / 100; --hi)
		;
	if (hi - lo < 32) {
		lo = 0;
		hi = 255;	/* nearly flat: leave it */
	}
	{
		int16_t *c = malloc(sizeof(int16_t) * (size_t)w * h);
		for (i = 0; i < w * h; ++i) {
			int v = (g[i] - lo) * 255 / (hi - lo);
			c[i] = (int16_t)(v < 0 ? 0 : v > 255 ? 255 : v);
		}
		for (y = 0; y < h; ++y)
			for (x = 0; x < w; ++x) {
				int v = c[y * w + x];
				/* unsharp mask, 3x3 box, amount 0.5 */
				if (x > 0 && y > 0 && x < w - 1 && y < h - 1) {
					int s = 0, j, k;
					for (j = -1; j <= 1; ++j)
						for (k = -1; k <= 1; ++k)
							s += c[(y + j) * w + x + k];
					v = v + (v * 9 - s) * 5 / 90;
					v = v < 0 ? 0 : v > 255 ? 255 : v;
				}
				e[y * w + x] = lin(v);
			}
		free(c);
	}
	for (y = 0; y < h; ++y) {
		int ltr = !(y & 1), xs = ltr ? 0 : w - 1, dx = ltr ? 1 : -1;
		for (x = xs; x >= 0 && x < w; x += dx) {
			float old = e[y * w + x], err, best = 9;
			int k, q = 0;
			for (k = 0; k < 4; ++k) {
				float dd = fabsf(old - pal[k]);
				if (dd < best) {
					best = dd;
					q = k;
				}
			}
			err = old - pal[q];
			bits[y * stride + (x >> 2)] |= (uint8_t)(q << (6 - 2 * (x & 3)));
			if (x + dx >= 0 && x + dx < w)
				e[y * w + x + dx] += err * 7 / 16;
			if (y + 1 < h) {
				if (x - dx >= 0 && x - dx < w)
					e[(y + 1) * w + x - dx] += err * 3 / 16;
				e[(y + 1) * w + x] += err * 5 / 16;
				if (x + dx >= 0 && x + dx < w)
					e[(y + 1) * w + x + dx] += err / 16;
			}
		}
	}
	free(e);
	return bits;
}

static void convert(job_t *j)
{
	uint8_t *rgba = NULL, *g, *bits;
	int sw = 0, sh = 0, tw, th;
	uint32_t sz;
	size_t cap, n;
	j->rec = NULL;
	j->w = j->h = 0;
	if (j->raw_len >= 12 && !memcmp(j->raw, "RIFF", 4) && !memcmp(j->raw + 8, "WEBP", 4)) {
		WebPDecoderConfig cfg;
		if (!WebPGetInfo(j->raw, j->raw_len, &sw, &sh) || sw <= 0 || sh <= 0 ||
		    (double)sw * sh > 100e6 || !WebPInitDecoderConfig(&cfg))
			return;
		fit(sw, sh, &tw, &th);
		if (tw < MIN_SIDE || th < MIN_SIDE)
			return;
		cfg.options.use_scaling = tw != sw || th != sh;
		cfg.options.scaled_width = tw;
		cfg.options.scaled_height = th;
		cfg.output.colorspace = MODE_RGBA;
		if (WebPDecode(j->raw, j->raw_len, &cfg) != VP8_STATUS_OK)
			return;
		g = to_grey(cfg.output.u.RGBA.rgba, tw, th, tw, th);
		WebPFreeDecBuffer(&cfg.output);
	} else {
		int ch;
		rgba = stbi_load_from_memory(j->raw, (int)j->raw_len, &sw, &sh, &ch, 4);
		if (!rgba)
			return;
		fit(sw, sh, &tw, &th);
		if (tw < MIN_SIDE || th < MIN_SIDE) {
			stbi_image_free(rgba);
			return;
		}
		g = to_grey(rgba, sw, sh, tw, th);
		stbi_image_free(rgba);
	}
	bits = dither4(g, tw, th);
	free(g);
	j->w = (uint16_t)tw;
	j->h = (uint16_t)th;
	/* the record: u16 w, u16 h, u8 format (0 as is, 1 zstd), 3 x 0, data;
	 * zstd (no dictionary) when it saves something and fits the
	 * PicoCalc's compressed buffer */
	sz = (uint32_t)(((tw * 2 + 7) >> 3) * th);
	cap = 8 + ZSTD_compressBound(sz);
	j->rec = malloc(cap);
	n = ZSTD_compress(j->rec + 8, cap - 8, bits, sz, 19);
	memset(j->rec, 0, 8);
	zw_wr16(j->rec, (uint16_t)tw);
	zw_wr16(j->rec + 2, (uint16_t)th);
	if (!ZSTD_isError(n) && n + 16 < sz && n <= PIC_ZMAX) {
		j->rec[4] = 1;
		j->rec_len = 8 + (uint32_t)n;
	} else {
		memcpy(j->rec + 8, bits, sz);
		j->rec_len = 8 + sz;
	}
	free(bits);
}

typedef struct pool {
	pthread_mutex_t m;
	pthread_cond_t go, done;
	job_t *jobs;
	int njobs, next, finished, quit, gen;
} pool_t;

static void *worker(void *arg)
{
	pool_t *p = arg;
	int gen = 0;
	pthread_mutex_lock(&p->m);
	for (;;) {
		while (!p->quit && (p->gen == gen || p->next >= p->njobs))
			pthread_cond_wait(&p->go, &p->m);
		if (p->quit)
			break;
		while (p->next < p->njobs) {
			job_t *j = &p->jobs[p->next++];
			pthread_mutex_unlock(&p->m);
			convert(j);
			pthread_mutex_lock(&p->m);
			if (++p->finished == p->njobs)
				pthread_cond_signal(&p->done);
		}
		gen = p->gen;
	}
	pthread_mutex_unlock(&p->m);
	return NULL;
}

/* ------------------------------------------------------------------ */
/* step 3: NAME.I00 ... in pieces of 2 GiB */

typedef struct pw {
	char base[4200];
	int fd[100];
	uint64_t size;
} pw_t;

static void pw_write(pw_t *w, uint64_t pos, const void *buf, size_t len)
{
	const uint8_t *b = buf;
	while (len) {
		uint32_t c = (uint32_t)(pos >> 31);
		uint64_t in = pos - ((uint64_t)c << 31);
		size_t n = (size_t)(((uint64_t)1 << 31) - in);
		if (n > len)
			n = len;
		if (c >= 100) {
			fprintf(stderr, "zimprep: picture files too big\n");
			exit(1);
		}
		if (w->fd[c] < 0) {
			char path[4300];
			snprintf(path, sizeof(path), "%s.I%02u", w->base, c);
			w->fd[c] = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
			if (w->fd[c] < 0) {
				perror(path);
				exit(1);
			}
		}
		if (pwrite(w->fd[c], b, n, (off_t)in) != (ssize_t)n) {
			perror("zimprep: writing pictures");
			exit(1);
		}
		pos += n;
		b += n;
		len -= n;
		if (pos > w->size)
			w->size = pos;
	}
}

static int cmp_img_entry(const void *a, const void *b)
{
	const uint32_t x = imgs[*(const uint32_t *)a].entry, y = imgs[*(const uint32_t *)b].entry;
	return x < y ? -1 : x > y;
}

long pictures_make(zim_t *z, const char *input, const char *base, int mode, int threads)
{
	uint32_t i, *order, done_imgs = 0;
	uint64_t pos, data_start, bytes_out = 0, raw_total = 0;
	uint8_t *table;
	pw_t w;
	pool_t pool;
	pthread_t th[64];
	uint8_t hdr[PIC_FILE_HDR];
	int t;
	char rm[4300];

	for (i = 0; i < 100; ++i) {
		snprintf(rm, sizeof(rm), "%s.I%02u", base, i);
		unlink(rm);	/* pieces of an older run */
	}
	if (mode == PICS_NONE)
		return 0;
	img_tab_mask = 4095;
	img_tab = calloc(img_tab_mask + 1, sizeof(uint32_t));
	if (threads < 1)
		threads = 1;
	if (threads > 64)
		threads = 64;
	/* 1. scan */
	{
		scan_t sc;
		uint32_t b, k;
		memset(&sc, 0, sizeof(sc));
		sc.input = input;
		sc.mode = mode;
		sc.entries = z->entry_count;
		sc.nblocks = (z->entry_count + SCAN_BLOCK - 1) / SCAN_BLOCK;
		sc.res = calloc(sc.nblocks ? sc.nblocks : 1, sizeof(sres_t));
		pthread_mutex_init(&sc.m, NULL);
		pthread_cond_init(&sc.done, NULL);
		for (t = 0; t < threads; ++t)
			pthread_create(&th[t], NULL, scan_worker, &sc);
		for (b = 0; b < sc.nblocks; ++b) {
			sres_t *r = &sc.res[b];
			uint32_t at = 0;
			pthread_mutex_lock(&sc.m);
			while (!r->done)
				pthread_cond_wait(&sc.done, &sc.m);
			pthread_mutex_unlock(&sc.m);
			for (k = 0; k < r->n; ++k) {
				add_article(r->entry[k], r->count[k], r->hash + at, r->pe + at);
				at += r->count[k];
			}
			free(r->entry);
			free(r->count);
			free(r->hash);
			free(r->pe);
			progress("finding pictures", (uint64_t)(b + 1) * SCAN_BLOCK < z->entry_count ?
				 (uint64_t)(b + 1) * SCAN_BLOCK : z->entry_count, z->entry_count);
		}
		for (t = 0; t < threads; ++t)
			pthread_join(th[t], NULL);
		free(sc.res);
	}
	fprintf(stderr, "%u articles with pictures, %u pictures to convert\n", narts, nimgs);

	memset(&w, 0, sizeof(w));
	for (i = 0; i < 100; ++i)
		w.fd[i] = -1;
	snprintf(w.base, sizeof(w.base), "%s", base);
	if (!nimgs)
		return 0;

	/* 2. convert, in the ZIM's order; the picture number is that order */
	order = malloc(sizeof(uint32_t) * nimgs);
	for (i = 0; i < nimgs; ++i)
		order[i] = i;
	qsort(order, nimgs, sizeof(uint32_t), cmp_img_entry);
	for (i = 0; i < nimgs; ++i)
		imgs[order[i]].number = i;
	table = calloc(nimgs, 8);
	data_start = PIC_FILE_HDR + (uint64_t)nimgs * 8;
	pos = data_start;
	memset(&pool, 0, sizeof(pool));
	pthread_mutex_init(&pool.m, NULL);
	pthread_cond_init(&pool.go, NULL);
	pthread_cond_init(&pool.done, NULL);
	pool.jobs = calloc(BATCH_IMGS, sizeof(job_t));
	for (t = 0; t < threads; ++t)
		pthread_create(&th[t], NULL, worker, &pool);
	while (done_imgs < nimgs) {
		int nj = 0, k;
		uint64_t bytes = 0;
		while (done_imgs < nimgs && nj < BATCH_IMGS && bytes < (uint64_t)BATCH_MB << 20) {
			img_t *im = &imgs[order[done_imgs++]];
			zim_dirent_t d;
			const uint8_t *data;
			uint32_t len;
			job_t *j = &pool.jobs[nj];
			memset(j, 0, sizeof(*j));
			j->img = (uint32_t)(im - imgs);
			if (zim_dirent(z, im->entry, &d) < 0 || zim_blob(z, d.cluster, d.blob, &data, &len) < 0)
				continue;
			j->raw = malloc(len ? len : 1);
			memcpy(j->raw, data, len);
			j->raw_len = len;
			bytes += len;
			nj++;
		}
		pthread_mutex_lock(&pool.m);
		pool.njobs = nj;
		pool.next = 0;
		pool.finished = 0;
		pool.gen++;
		pthread_cond_broadcast(&pool.go);
		while (pool.finished < pool.njobs)
			pthread_cond_wait(&pool.done, &pool.m);
		pthread_mutex_unlock(&pool.m);
		for (k = 0; k < nj; ++k) {
			job_t *j = &pool.jobs[k];
			img_t *im = &imgs[j->img];
			if (j->rec) {
				uint8_t *e = table + (size_t)im->number * 8;
				if (pos >> 40 || j->rec_len >= (1u << 24)) {
					fprintf(stderr, "zimprep: picture files too big\n");
					exit(1);
				}
				pw_write(&w, pos, j->rec, j->rec_len);
				zw_wr32(e, (uint32_t)pos);
				e[4] = (uint8_t)(pos >> 32);
				e[5] = (uint8_t)j->rec_len;
				e[6] = (uint8_t)(j->rec_len >> 8);
				e[7] = (uint8_t)(j->rec_len >> 16);
				im->w = j->w;
				im->h = j->h;
				pos += j->rec_len;
				bytes_out += j->rec_len;
				raw_total += 8u + (uint32_t)(((j->w * 2 + 7) >> 3) * j->h);
				nconverted++;
				free(j->rec);
			}
			free(j->raw);
		}
		progress("converting pictures", done_imgs, nimgs);
	}
	pthread_mutex_lock(&pool.m);
	pool.quit = 1;
	pthread_cond_broadcast(&pool.go);
	pthread_mutex_unlock(&pool.m);
	for (t = 0; t < threads; ++t)
		pthread_join(th[t], NULL);
	pw_write(&w, PIC_FILE_HDR, table, (size_t)nimgs * 8);
	memset(hdr, 0, sizeof(hdr));
	memcpy(hdr, "PWGI", 4);
	zw_wr32(hdr + 4, 1);
	zw_wr32(hdr + 8, nimgs);
	zw_wr32(hdr + 12, nconverted);
	zw_wr32(hdr + 16, (uint32_t)w.size);
	zw_wr32(hdr + 20, (uint32_t)(w.size >> 32));
	pw_write(&w, 0, hdr, PIC_FILE_HDR);
	for (i = 0; i < 100; ++i)
		if (w.fd[i] >= 0)
			close(w.fd[i]);
	fprintf(stderr, "pictures: %u (%u could not be used), %llu bytes in %s.I00... "
		"(%llu before compression)\n", nconverted, nimgs - nconverted,
		(unsigned long long)w.size, base, (unsigned long long)raw_total);
	free(order);
	free(pool.jobs);
	free(table);
	free(img_tab);
	img_tab = NULL;
	return (long)nimgs;
}

uint32_t pictures_count(void)
{
	return nimgs;
}

void pictures_for(uint32_t entry, pic_set_t *s)
{
	uint32_t lo = 0, hi = narts, k;
	memset(s, 0, sizeof(*s));
	while (lo < hi) {
		uint32_t mid = (lo + hi) / 2;
		if (arts[mid].entry < entry)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo >= narts || arts[lo].entry != entry)
		return;
	for (k = 0; k < arts[lo].n && s->n < PIC_MAX; ++k) {
		const ref_t *r = &refs[arts[lo].first + k];
		const img_t *im = &imgs[r->img];
		if (!im->w)
			continue;	/* could not be converted */
		s->hash[s->n] = r->hash;
		s->w[s->n] = im->w;
		s->h[s->n] = im->h;
		s->off[s->n] = im->number;
		s->n++;
	}
}
