/*
 * PicoWikiGuide PC builds: a heap like the PicoCalc's (PWG_HEAP=BYTES)
 */
#ifndef HEAPEMU_H
#define HEAPEMU_H

#include <stddef.h>
#include <stdint.h>

int heapemu_on(void);		/* PWG_HEAP set: use the arena */
void *heapemu_alloc(size_t n);
void heapemu_free(void *p);
void *heapemu_realloc(void *p, size_t n);
void heapemu_stats(uint32_t *free_bytes, uint32_t *largest);

#endif
