/*
 * PicoWikiGuide PC builds: a heap like the PicoCalc's, for tests.
 *
 * PWG_HEAP=BYTES in the environment makes zw_alloc and friends use a
 * fixed arena of that size, allocated roughly the way newlib's malloc
 * does on the Pico: 8 byte headers, 8 byte alignment, 16 byte minimum
 * chunks, best fit from the free chunks, else the top of the heap grows
 * (like sbrk) until the arena is used up, freed chunks merged with their
 * neighbours and given back to the top.  So a test can see running out
 * of memory, holes (free memory in pieces) and leaks, which the PC's own
 * malloc hides.  Not exact (newlib's bins differ in detail) but close.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "heapemu.h"

#define HDR   8
#define ALIGN 8
#define MINC  16

static uint8_t *arena;
static size_t arena_len, top;	/* top: start of the never used part */
static int ready = -1;		/* -1 not checked, 0 off, 1 on */

/* chunk header: size (with header) in the low bits, bit 0 = in use;
 * plus the size of the chunk before (for merging backwards) */
typedef struct { uint32_t size, prev; } chunk_t;

static chunk_t *at(size_t off) { return (chunk_t *)(arena + off); }

int heapemu_on(void)
{
	if (ready < 0) {
		const char *s = getenv("PWG_HEAP");
		ready = 0;
		if (s && atol(s) > 1024) {
			arena_len = (size_t)atol(s) & ~(size_t)(ALIGN - 1);
			arena = calloc(1, arena_len);
			top = 0;
			ready = arena != NULL;
		}
	}
	return ready;
}

static size_t chunk_size(size_t n)
{
	size_t c = (n + HDR + ALIGN - 1) & ~(size_t)(ALIGN - 1);
	return c < MINC ? MINC : c;
}

void *heapemu_alloc(size_t n)
{
	size_t need = chunk_size(n ? n : 1), off = 0, best = (size_t)-1, bsize = (size_t)-1;
	/* best fit among the free chunks */
	while (off < top) {
		chunk_t *c = at(off);
		size_t sz = c->size & ~1u;
		if (!(c->size & 1) && sz >= need && sz < bsize) {
			best = off;
			bsize = sz;
			if (sz == need)
				break;
		}
		off += sz;
	}
	if (best != (size_t)-1) {
		chunk_t *c = at(best);
		if (bsize - need >= MINC) {	/* split */
			chunk_t *rest = at(best + need), *after;
			rest->size = (uint32_t)(bsize - need);
			rest->prev = (uint32_t)need;
			if (best + bsize < top) {
				after = at(best + bsize);
				after->prev = rest->size;
			}
			c->size = (uint32_t)need | 1;
		} else {
			c->size |= 1;
		}
		return arena + best + HDR;
	}
	/* grow the top (merging a free last chunk first, as sbrk would) */
	if (top + need > arena_len)
		return NULL;
	{
		chunk_t *c = at(top);
		uint32_t prev = 0;
		if (top) {	/* size of the chunk before: walk is fine for tests */
			size_t o = 0, last = 0;
			while (o < top) {
				last = o;
				o += at(o)->size & ~1u;
			}
			prev = (uint32_t)(top - last);
		}
		c->size = (uint32_t)need | 1;
		c->prev = prev;
		top += need;
		return arena + top - need + HDR;
	}
}

void heapemu_free(void *p)
{
	size_t off;
	chunk_t *c;
	if (!p)
		return;
	off = (size_t)((uint8_t *)p - arena) - HDR;
	c = at(off);
	if (off >= top || !(c->size & 1)) {
		fprintf(stderr, "heapemu: bad free\n");
		abort();
	}
	c->size &= ~1u;
	/* merge with the next chunk */
	if (off + c->size < top && !(at(off + c->size)->size & 1))
		c->size += at(off + c->size)->size;
	/* and with the one before */
	if (off && c->prev && !(at(off - c->prev)->size & 1)) {
		chunk_t *b = at(off - c->prev);
		b->size += c->size;
		off -= c->prev;
		c = b;
	}
	if (off + c->size >= top) {	/* the last chunk: back to the top */
		top = off;
		return;
	}
	at(off + c->size)->prev = c->size;
}

void *heapemu_realloc(void *p, size_t n)
{
	size_t off, sz, need;
	void *q;
	if (!p)
		return heapemu_alloc(n);
	off = (size_t)((uint8_t *)p - arena) - HDR;
	sz = at(off)->size & ~1u;
	need = chunk_size(n);
	if (need <= sz)
		return p;
	/* the last chunk: grow in place */
	if (off + sz == top && off + need <= arena_len) {
		top = off + need;
		at(off)->size = (uint32_t)need | 1;
		return p;
	}
	q = heapemu_alloc(n);
	if (!q)
		return NULL;
	memcpy(q, p, sz - HDR);
	heapemu_free(p);
	return q;
}

void heapemu_stats(uint32_t *free_bytes, uint32_t *largest)
{
	size_t off = 0, fr = 0, big = 0;
	while (off < top) {
		chunk_t *c = at(off);
		size_t sz = c->size & ~1u;
		if (!(c->size & 1)) {
			fr += sz;
			if (sz - HDR > big)
				big = sz - HDR;
		}
		off += sz;
	}
	fr += arena_len - top;
	if (arena_len - top > HDR && arena_len - top - HDR > big)
		big = arena_len - top - HDR;
	*free_bytes = (uint32_t)fr;
	*largest = (uint32_t)big;
}
