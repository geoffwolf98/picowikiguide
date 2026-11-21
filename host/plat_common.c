/*
 * PicoWikiGuide PC builds: key names, frame buffer to picture files
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "plat.h"
#include "gfx.h"
#include "plat_common.h"

static const struct { const char *name; int key; } keys[] = {
	{ "up", K_UP }, { "down", K_DOWN }, { "left", K_LEFT }, { "right", K_RIGHT },
	{ "enter", K_ENTER }, { "esc", K_ESC }, { "bs", K_BS }, { "tab", K_TAB },
	{ "del", K_DEL }, { "home", K_HOME }, { "end", K_END }, { "pgup", K_PGUP },
	{ "pgdn", K_PGDN }, { "f1", K_F1 }, { "f2", K_F2 }, { "f3", K_F3 },
	{ "f4", K_F4 }, { "f5", K_F5 }, { "space", ' ' }, { "ins", K_INS },
};

int host_key_by_name(const char *s, int *mods)
{
	unsigned i;
	*mods = 0;
	for (;;) {
		if (strncasecmp(s, "shift+", 6) == 0) { *mods |= KM_SHIFT; s += 6; }
		else if (strncasecmp(s, "ctrl+", 5) == 0) { *mods |= KM_CTRL; s += 5; }
		else if (strncasecmp(s, "alt+", 4) == 0) { *mods |= KM_ALT; s += 4; }
		else break;
	}
	for (i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i)
		if (strcasecmp(s, keys[i].name) == 0)
			return keys[i].key;
	if (s[0] && !s[1])
		return (unsigned char)s[0];
	return -1;
}

/* PNG, 2 bit grey, deflate "stored" blocks: no zlib needed */
static uint32_t crc_tab[256];
static uint32_t crc(uint32_t c, const uint8_t *p, size_t n)
{
	size_t i;
	if (!crc_tab[1]) {
		uint32_t k, j, v;
		for (k = 0; k < 256; ++k) {
			for (v = k, j = 0; j < 8; ++j)
				v = (v & 1) ? 0xedb88320u ^ (v >> 1) : v >> 1;
			crc_tab[k] = v;
		}
	}
	for (i = 0; i < n; ++i)
		c = crc_tab[(c ^ p[i]) & 0xff] ^ (c >> 8);
	return c;
}

static void be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static void chunk(FILE *f, const char *type, const uint8_t *d, uint32_t n)
{
	uint8_t h[8], t[4];
	uint32_t c;
	be32(h, n);
	memcpy(h + 4, type, 4);
	fwrite(h, 1, 8, f);
	fwrite(d, 1, n, f);
	c = crc(0xffffffffu, h + 4, 4);
	c = crc(c, d, n) ^ 0xffffffffu;
	be32(t, c);
	fwrite(t, 1, 4, f);
}

static int save_png(const char *path, const uint8_t *fb)
{
	static uint8_t raw[(SCREEN_STRIDE + 1) * SCREEN_H], z[sizeof(raw) + 64];
	uint8_t ihdr[13] = { 0 };
	uint32_t n = 0, i, a = 1, b = 0, left, zn = 0;
	FILE *f = fopen(path, "wb");
	if (!f)
		return -1;
	for (i = 0; i < SCREEN_H; ++i) {
		uint32_t k;
		raw[n++] = 0;
		for (k = 0; k < SCREEN_STRIDE; ++k)
			raw[n++] = (uint8_t)~fb[i * SCREEN_STRIDE + k];	/* PNG: 3 = white */
	}
	z[zn++] = 0x78;
	z[zn++] = 0x01;
	for (i = 0; i < n; ) {
		uint32_t len = n - i > 65535 ? 65535 : n - i;
		z[zn++] = (uint8_t)(i + len == n);
		z[zn++] = (uint8_t)len; z[zn++] = (uint8_t)(len >> 8);
		z[zn++] = (uint8_t)~len; z[zn++] = (uint8_t)(~len >> 8);
		memcpy(z + zn, raw + i, len);
		zn += len;
		i += len;
	}
	for (i = 0, left = n; i < n; ++i, --left) {
		a = (a + raw[i]) % 65521;
		b = (b + a) % 65521;
	}
	be32(z + zn, (b << 16) | a);
	zn += 4;
	fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
	be32(ihdr, SCREEN_W);
	be32(ihdr + 4, SCREEN_H);
	ihdr[8] = 2;	/* bit depth: 4 greys */
	ihdr[9] = 0;	/* grey */
	chunk(f, "IHDR", ihdr, 13);
	chunk(f, "IDAT", z, zn);
	chunk(f, "IEND", NULL, 0);
	return fclose(f);
}

int host_save_pbm(const char *path, const uint8_t *fb)
{
	size_t l = strlen(path);
	if (l > 4 && strcasecmp(path + l - 4, ".png") == 0)
		return save_png(path, fb);
	{
	FILE *f = fopen(path, "wb");
	if (!f)
		return -1;
	{
		/* PGM, 8 bits a pixel */
		int i;
		fprintf(f, "P5\n%d %d\n255\n", SCREEN_W, SCREEN_H);
		for (i = 0; i < SCREEN_W * SCREEN_H; ++i) {
			int v = (fb[(i / SCREEN_W) * SCREEN_STRIDE + (i % SCREEN_W) / 4] >> (6 - 2 * (i % 4))) & 3;
			fputc(255 - v * 85, f);
		}
		return fclose(f);
	}
	}
}

uint32_t plat_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint32_t)(t.tv_sec * 1000u + t.tv_nsec / 1000000u);
}

void plat_keepalive(void) {}
void plat_restart(void) {}

/* the Unicode fallback font: fonts/WIKIFONT.BIN next to build/ (or the
 * path in PWG_WIKIFONT); kept for the whole run */
#include <stdlib.h>
#include "font.h"
int host_load_wikifont(const char *argv0)
{
	char path[4096];
	const char *env = getenv("PWG_WIKIFONT");
	FILE *f;
	long n;
	uint8_t *d;
	if (env)
		snprintf(path, sizeof(path), "%s", env);
	else {
		const char *slash = strrchr(argv0, '/');
		int dl = slash ? (int)(slash - argv0) : 1;
		snprintf(path, sizeof(path), "%.*s/../fonts/WIKIFONT.BIN", dl, slash ? argv0 : ".");
	}
	if (!(f = fopen(path, "rb"))) {
		fprintf(stderr, "cannot read the Unicode font %s (set PWG_WIKIFONT)\n", path);
		return -1;
	}
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	d = malloc((size_t)n);
	if (!d || fread(d, 1, (size_t)n, f) != (size_t)n || font_set_fallback(d, (uint32_t)n) < 0) {
		fclose(f);
		fprintf(stderr, "%s is not a usable font file\n", path);
		return -1;
	}
	fclose(f);
	return 0;
}
