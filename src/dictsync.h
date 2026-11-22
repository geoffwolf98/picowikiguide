/*
 * PicoWikiGuide - keep a copy of a wiki's dictionary in flash
 *
 * The dictionary (up to 112 KB) is only read once it is loaded, so on
 * the PicoCalc it lives in flash rather than RAM.  dictsync() makes the
 * flash area equal to the dictionary on the card, sector by sector,
 * erasing and writing only the sectors that differ: the same wiki opened
 * again writes nothing, and a copy cut short (power off) is finished the
 * next time.  The flash itself is behind callbacks, so the logic is
 * tested on the PC with a fake flash.
 */
#ifndef DICTSYNC_H
#define DICTSYNC_H

#include "zw.h"

typedef struct {
	const uint8_t *area;	/* the flash area, readable */
	uint32_t area_len;	/* a multiple of sector */
	uint32_t sector;	/* erase size (4096) */
	/* offsets are from the start of the area; return < 0 on failure */
	int (*erase)(void *ctx, uint32_t off, uint32_t len);
	int (*program)(void *ctx, uint32_t off, const uint8_t *buf, uint32_t len);
	int (*read)(void *ctx, uint32_t off, uint8_t *buf, uint32_t len);	/* the card */
	void (*notice)(void *ctx);	/* before the first write, if any */
	void *ctx;
	uint8_t *tmp;		/* a buffer of a multiple of sector bytes */
	uint32_t tmp_len;
	uint32_t written;	/* out: sectors written */
} dictsync_t;

/* make area[0..len) equal to the dictionary (len <= area_len); the rest
 * of its last sector is 0xff.  Returns ZW_OK or an error. */
int dictsync(dictsync_t *d, uint32_t len);

#endif
