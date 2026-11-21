/*
 * PicoWikiGuide - a small raw deflate (RFC 1951) decoder, for the
 * compressed Unicode fallback font.  After Mark Adler's puff.c (zlib
 * licence).  Tables are static (not on the 4 KB stack); not reentrant.
 */
#include "inflate.h"

#define MAXBITS  15
#define MAXLCODES 286
#define MAXDCODES 30
#define FIXLCODES 288

typedef struct {
	const uint8_t *in;
	uint32_t inlen, inpos;
	uint32_t bitbuf;
	int bitcnt;
	uint8_t *out;
	uint32_t outlen, outpos;
	int err;
} st_t;

typedef struct {
	int16_t count[MAXBITS + 1];
	int16_t symbol[FIXLCODES];
} huff_t;

static huff_t lencode, distcode;
static int16_t lengths[MAXLCODES + MAXDCODES];

static int bits(st_t *s, int need)
{
	uint32_t val = s->bitbuf;
	while (s->bitcnt < need) {
		if (s->inpos >= s->inlen) {
			s->err = 1;
			return 0;
		}
		val |= (uint32_t)s->in[s->inpos++] << s->bitcnt;
		s->bitcnt += 8;
	}
	s->bitbuf = val >> need;
	s->bitcnt -= need;
	return (int)(val & ((1u << need) - 1));
}

static int stored(st_t *s)
{
	uint32_t len;
	s->bitbuf = 0;
	s->bitcnt = 0;
	if (s->inpos + 4 > s->inlen)
		return -1;
	len = s->in[s->inpos] | ((uint32_t)s->in[s->inpos + 1] << 8);
	if ((s->in[s->inpos + 2] ^ 0xff) != (len & 0xff) ||
	    (s->in[s->inpos + 3] ^ 0xff) != (len >> 8))
		return -1;
	s->inpos += 4;
	if (s->inpos + len > s->inlen || s->outpos + len > s->outlen)
		return -1;
	memcpy(s->out + s->outpos, s->in + s->inpos, len);
	s->inpos += len;
	s->outpos += len;
	return 0;
}

static int decode(st_t *s, const huff_t *h)
{
	int len, code = 0, first = 0, count, index = 0;
	for (len = 1; len <= MAXBITS; len++) {
		code |= bits(s, 1);
		if (s->err)
			return -1;
		count = h->count[len];
		if (code - count < first)
			return h->symbol[index + (code - first)];
		index += count;
		first += count;
		first <<= 1;
		code <<= 1;
	}
	return -1;
}

/* 0: complete, >0: incomplete (allowed for single codes), <0: bad */
static int construct(huff_t *h, const int16_t *length, int n)
{
	int sym, len, left;
	int16_t offs[MAXBITS + 1];
	for (len = 0; len <= MAXBITS; len++)
		h->count[len] = 0;
	for (sym = 0; sym < n; sym++)
		h->count[length[sym]]++;
	if (h->count[0] == n)
		return 0;
	left = 1;
	for (len = 1; len <= MAXBITS; len++) {
		left <<= 1;
		left -= h->count[len];
		if (left < 0)
			return left;
	}
	offs[1] = 0;
	for (len = 1; len < MAXBITS; len++)
		offs[len + 1] = (int16_t)(offs[len] + h->count[len]);
	for (sym = 0; sym < n; sym++)
		if (length[sym] != 0)
			h->symbol[offs[length[sym]]++] = (int16_t)sym;
	return left;
}

static int codes(st_t *s, const huff_t *lc, const huff_t *dc)
{
	static const uint16_t lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
					    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
	static const uint8_t lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
					  3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
	static const uint16_t dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
					    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
					    8193, 12289, 16385, 24577 };
	static const uint8_t dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
					  7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
	int sym;
	uint32_t len, dist;
	for (;;) {
		sym = decode(s, lc);
		if (sym < 0)
			return -1;
		if (sym < 256) {
			if (s->outpos >= s->outlen)
				return -1;
			s->out[s->outpos++] = (uint8_t)sym;
		} else if (sym == 256) {
			return 0;
		} else {
			sym -= 257;
			if (sym >= 29)
				return -1;
			len = lbase[sym] + (uint32_t)bits(s, lext[sym]);
			sym = decode(s, dc);
			if (sym < 0 || sym >= 30)
				return -1;
			dist = dbase[sym] + (uint32_t)bits(s, dext[sym]);
			if (s->err || dist > s->outpos || s->outpos + len > s->outlen)
				return -1;
			while (len--) {
				s->out[s->outpos] = s->out[s->outpos - dist];
				s->outpos++;
			}
		}
	}
}

static int fixed(st_t *s)
{
	static int built;
	static huff_t flen, fdist;
	if (!built) {
		int sym;
		for (sym = 0; sym < 144; sym++)
			lengths[sym] = 8;
		for (; sym < 256; sym++)
			lengths[sym] = 9;
		for (; sym < 280; sym++)
			lengths[sym] = 7;
		for (; sym < FIXLCODES; sym++)
			lengths[sym] = 8;
		construct(&flen, lengths, FIXLCODES);
		for (sym = 0; sym < MAXDCODES; sym++)
			lengths[sym] = 5;
		construct(&fdist, lengths, MAXDCODES);
		built = 1;
	}
	return codes(s, &flen, &fdist);
}

static int dynamic(st_t *s)
{
	static const uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
	int nlen, ndist, ncode, index, err;
	nlen = bits(s, 5) + 257;
	ndist = bits(s, 5) + 1;
	ncode = bits(s, 4) + 4;
	if (s->err || nlen > MAXLCODES || ndist > MAXDCODES)
		return -1;
	for (index = 0; index < ncode; index++)
		lengths[order[index]] = (int16_t)bits(s, 3);
	for (; index < 19; index++)
		lengths[order[index]] = 0;
	if (s->err || construct(&lencode, lengths, 19) != 0)
		return -1;
	index = 0;
	while (index < nlen + ndist) {
		int sym = decode(s, &lencode), len = 0, rep;
		if (sym < 0)
			return -1;
		if (sym < 16) {
			lengths[index++] = (int16_t)sym;
			continue;
		}
		if (sym == 16) {
			if (index == 0)
				return -1;
			len = lengths[index - 1];
			rep = 3 + bits(s, 2);
		} else if (sym == 17) {
			rep = 3 + bits(s, 3);
		} else {
			rep = 11 + bits(s, 7);
		}
		if (s->err || index + rep > nlen + ndist)
			return -1;
		while (rep--)
			lengths[index++] = (int16_t)len;
	}
	if (lengths[256] == 0)
		return -1;
	err = construct(&lencode, lengths, nlen);
	if (err < 0 || (err > 0 && nlen - lencode.count[0] != 1))
		return -1;
	err = construct(&distcode, lengths + nlen, ndist);
	if (err < 0 || (err > 0 && ndist - distcode.count[0] != 1))
		return -1;
	return codes(s, &lencode, &distcode);
}

long zw_inflate(uint8_t *out, uint32_t outlen, const uint8_t *in, uint32_t inlen)
{
	st_t s;
	int last, type, r;
	memset(&s, 0, sizeof(s));
	s.in = in;
	s.inlen = inlen;
	s.out = out;
	s.outlen = outlen;
	do {
		last = bits(&s, 1);
		type = bits(&s, 2);
		if (s.err)
			return -1;
		r = type == 0 ? stored(&s) : type == 1 ? fixed(&s) : type == 2 ? dynamic(&s) : -1;
		if (r < 0 || s.err)
			return -1;
	} while (!last);
	return (long)s.outpos;
}
