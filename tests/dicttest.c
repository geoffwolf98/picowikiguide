/*
 * PicoWikiGuide - dictsync on a fake NOR flash: erase sets 0xff, writing
 * can only clear bits (as real flash), so a write without an erase shows
 * up as a wrong copy.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dictsync.h"

#define AREA (112 * 1024)
static uint8_t flash[AREA], src[AREA];
static int erases, fail_after = -1;

static int erase(void *c, uint32_t off, uint32_t len)
{
	(void)c;
	if (off % 4096 || len % 4096 || off + len > AREA)
		return -1;
	memset(flash + off, 0xff, len);
	erases++;
	return 0;
}

static int program(void *c, uint32_t off, const uint8_t *b, uint32_t len)
{
	uint32_t i;
	(void)c;
	if (fail_after == 0)
		return -1;	/* power cut */
	if (fail_after > 0)
		fail_after--;
	if (off % 256 || len % 256 || off + len > AREA)
		return -1;
	for (i = 0; i < len; ++i)
		flash[off + i] &= b[i];
	return 0;
}

static int rd(void *c, uint32_t off, uint8_t *b, uint32_t len)
{
	(void)c;
	memcpy(b, src + off, len);
	return 0;
}

static int run(uint32_t len, uint32_t *written)
{
	static uint8_t tmp[16384];
	dictsync_t d;
	int r;
	memset(&d, 0, sizeof(d));
	d.area = flash;
	d.area_len = AREA;
	d.sector = 4096;
	d.erase = erase;
	d.program = program;
	d.read = rd;
	d.tmp = tmp;
	d.tmp_len = sizeof(tmp);
	r = dictsync(&d, len);
	*written = d.written;
	return r;
}

static int check(const char *what, int ok)
{
	printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	return ok ? 0 : 1;
}

int main(void)
{
	uint32_t i, w, len = 114688, len2 = 100000;
	int bad = 0;
	memset(flash, 0x5a, sizeof(flash));	/* whatever was there */
	srand(1);
	for (i = 0; i < AREA; ++i)
		src[i] = (uint8_t)rand();
	bad += check("first copy writes every sector",
		     run(len, &w) == 0 && w == 28 && memcmp(flash, src, len) == 0);
	bad += check("the same dictionary again writes nothing", run(len, &w) == 0 && w == 0);
	src[5000] ^= 1;
	src[70000] ^= 0x80;
	bad += check("two bytes changed: two sectors written",
		     run(len, &w) == 0 && w == 2 && memcmp(flash, src, len) == 0);
	for (i = 0; i < AREA; ++i)
		src[i] = (uint8_t)rand();
	fail_after = 10;
	bad += check("power cut part way: an error", run(len, &w) < 0 && w == 10);
	fail_after = -1;
	bad += check("next time it is finished",
		     run(len, &w) == 0 && w == 18 && memcmp(flash, src, len) == 0);
	bad += check("a shorter dictionary (padded with 0xff)",
		     run(len2, &w) == 0 && memcmp(flash, src, len2) == 0 &&
		     flash[len2] == 0xff && flash[(len2 + 4095) / 4096 * 4096 - 1] == 0xff);
	bad += check("too long refused", run(AREA + 1, &w) < 0);
	return bad != 0;
}
