/*
 * PicoWikiGuide - files on the SD card for the PicoCalc platform:
 * read-only FAT (Gen2's fat.c) plus overwriting existing small files in
 * place.  Needs only sd_read and sd_write, so the PC tests can run it
 * on a card image.
 */
#include <stdio.h>
#include <string.h>
#include "zw.h"
#include "plat.h"
#include "fat.h"
#include "blockdev.h"
#include "sd.h"
#include "plat_storage.h"

#define MAX_PIECES  100

static fat_vol_t vol;

int storage_mount(void)
{
	return fat_mount(&vol);
}


typedef struct pfile {
	int n;			/* pieces */
	uint32_t shift;
	fat_file_t f[1];	/* n of them */
} pfile_t;

static int pf_read(void *ctx, uint64_t off, void *buf, uint32_t len)
{
	pfile_t *p = ctx;
	uint8_t *out = buf;
	while (len) {
		uint32_t i = p->n == 1 ? 0 : (uint32_t)(off >> p->shift);
		uint64_t in = p->n == 1 ? off : off - ((uint64_t)i << p->shift);
		uint32_t n = len;
		int r;
		if (i >= (uint32_t)p->n || in >= p->f[i].size)
			return ZW_ERANGE;
		if (in + n > p->f[i].size)
			n = (uint32_t)(p->f[i].size - in);
		if ((r = fat_read(&vol, &p->f[i], (uint32_t)in, out, n)) < 0)
			return r;
		out += n;
		off += n;
		len -= n;
	}
	return 0;
}

static pfile_t *pf_new(int n)
{
	pfile_t *p = zw_calloc(sizeof(pfile_t) + (size_t)(n - 1) * sizeof(fat_file_t));
	if (p)
		p->n = n;
	return p;
}

static void pf_free(pfile_t *p)
{
	int i;
	if (!p)
		return;
	for (i = 0; i < p->n; ++i)
		fat_close(&p->f[i]);
	zw_free(p);
}

static int open_one(const char *path, fat_file_t *f)
{
	if (fat_lookup(&vol, path, f) < 0)
		return ZW_ENOTFOUND;
	return fat_build_extents(&vol, f, NULL, NULL);
}

int plat_open(const char *path, zw_io_t *io)
{
	pfile_t *p = pf_new(1);
	if (!p)
		return ZW_ENOMEM;
	if (open_one(path, &p->f[0]) < 0) {
		pf_free(p);
		return ZW_ENOTFOUND;
	}
	io->read = pf_read;
	io->ctx = p;
	io->size = p->f[0].size;
	return 0;
}

int plat_open_pieces(const char *base, zw_io_t *io)
{
	char path[48];
	int n = 0, i;
	pfile_t *p;
	fat_file_t t;
	/* count the pieces */
	for (n = 0; n < MAX_PIECES; ++n) {
		snprintf(path, sizeof(path), "%s.P%02d", base, n);
		if (fat_lookup(&vol, path, &t) < 0)
			break;
	}
	if (!n || !(p = pf_new(n)))
		return ZW_ENOTFOUND;
	p->shift = 31;
	io->size = 0;
	for (i = 0; i < n; ++i) {
		snprintf(path, sizeof(path), "%s.P%02d", base, i);
		if (open_one(path, &p->f[i]) < 0) {
			pf_free(p);
			return ZW_EIO;
		}
		/* every piece but the last is exactly 2 GiB */
		if (i < n - 1 && p->f[i].size != (1u << 31)) {
			pf_free(p);
			return ZW_EFORMAT;
		}
	}
	io->read = pf_read;
	io->ctx = p;
	io->size = ((uint64_t)(n - 1) << 31) + p->f[n - 1].size;
	return 0;
}

void plat_close(zw_io_t *io)
{
	pf_free(io->ctx);
	memset(io, 0, sizeof(*io));
}

typedef struct list_ctx {
	char (*names)[16];
	int n, max;
} list_ctx_t;

static int list_cb(void *arg, const fat_file_t *f)
{
	list_ctx_t *c = arg;
	if ((f->attr & 0x10) && f->name[0] != '.' && c->n < c->max && strlen(f->name) <= 8)
		strcpy(c->names[c->n++], f->name);
	return 0;
}

int plat_list_wikis(char names[][16], int max)
{
	char cand[64][16], path[48];
	list_ctx_t c = { cand, 0, 64 };
	int i, n = 0, j;
	fat_file_t t;
	fat_list(&vol, "", list_cb, &c);
	for (i = 0; i < c.n && n < max; ++i) {
		snprintf(path, sizeof(path), "%s/%s.TOC", cand[i], cand[i]);
		if (fat_lookup(&vol, path, &t) == 0)
			strcpy(names[n++], cand[i]);
	}
	for (i = 1; i < n; ++i)
		for (j = i; j > 0 && strcmp(names[j - 1], names[j]) > 0; --j) {
			char s[16];
			strcpy(s, names[j]);
			strcpy(names[j], names[j - 1]);
			strcpy(names[j - 1], s);
		}
	return n;
}

int plat_load(const char *path, uint8_t **data, uint32_t *len)
{
	return plat_load_max(path, 0xffffffffu, data, len);
}

int plat_load_max(const char *path, uint32_t max, uint8_t **data, uint32_t *len)
{
	fat_file_t f;
	int r;
	if (open_one(path, &f) < 0)
		return ZW_ENOTFOUND;
	if (f.size > max)
		f.size = max;
	*data = zw_alloc(f.size + 1);
	if (!*data) {
		fat_close(&f);
		return ZW_ENOMEM;
	}
	r = f.size ? fat_read(&vol, &f, 0, *data, f.size) : 0;
	*len = f.size;
	fat_close(&f);
	if (r < 0) {
		zw_free(*data);
		*data = NULL;
	}
	return r;
}

/* write into an existing file's own clusters: the FAT and the directory
 * are never touched, so the file system cannot be damaged */
int plat_save(const char *path, const void *data, uint32_t len)
{
	static uint8_t sec[512];
	const uint8_t *in = data;
	fat_file_t f;
	uint32_t off = 0, cshift = 9 + vol.spc_shift;
	int r = 0;
	if (open_one(path, &f) < 0)
		return ZW_ENOTFOUND;
	if (len > f.size) {
		fat_close(&f);
		return ZW_ERANGE;
	}
	while (off < len && r == 0) {
		uint32_t fc = off >> cshift, k, lba, n;
		const fat_extent_t *e = NULL;
		for (k = 0; k < f.n_ext; ++k)
			if (fc >= f.ext[k].file_cluster && fc < f.ext[k].file_cluster + f.ext[k].count) {
				e = &f.ext[k];
				break;
			}
		if (!e) {
			r = ZW_EFORMAT;
			break;
		}
		lba = vol.data_lba + ((e->disk_cluster + (fc - e->file_cluster) - 2) << vol.spc_shift) +
		      ((off & ((1u << cshift) - 1)) >> 9);
		n = len - off < 512 ? len - off : 512;
		if (n < 512) {
			/* keep the rest of the last sector */
			if (sd_read(lba, sec, 1) < 0) {
				r = ZW_EIO;
				break;
			}
		}
		memcpy(sec, in + off, n);
		if (sd_write(lba, sec) < 0)
			r = ZW_EIO;
		off += 512;
	}
	fat_close(&f);
	bd_invalidate();
	return r;
}

