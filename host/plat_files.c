/*
 * PicoWikiGuide PC builds: the card is a folder on the PC
 */
#define _FILE_OFFSET_BITS 64
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "plat.h"
#include "hostio.h"
#include "plat_files.h"

static char card[4096] = ".";

void plat_files_set_card(const char *dir)
{
	snprintf(card, sizeof(card), "%s", dir);
}

static void full(char *out, size_t cap, const char *path)
{
	snprintf(out, cap, "%s/%s", card, path);
}

int plat_list_wikis(char names[][16], int max)
{
	DIR *d = opendir(card);
	struct dirent *e;
	int n = 0, i, j;
	if (!d)
		return 0;
	while ((e = readdir(d)) != NULL && n < max) {
		char p[4200];
		struct stat st;
		if (e->d_name[0] == '.' || strlen(e->d_name) > 8)
			continue;
		snprintf(p, sizeof(p), "%s/%s/%s.TOC", card, e->d_name, e->d_name);
		if (stat(p, &st) == 0) {
			strcpy(names[n++], e->d_name);
		}
	}
	closedir(d);
	/* sorted, like the device does */
	for (i = 1; i < n; ++i)
		for (j = i; j > 0 && strcmp(names[j - 1], names[j]) > 0; --j) {
			char t[16];
			strcpy(t, names[j]);
			strcpy(names[j], names[j - 1]);
			strcpy(names[j - 1], t);
		}
	return n;
}

int plat_open(const char *path, zw_io_t *io)
{
	char p[4200];
	host_files_t *f = malloc(sizeof(*f));
	if (!f)
		return ZW_ENOMEM;
	full(p, sizeof(p), path);
	if (host_files_open(f, p) < 0) {
		free(f);
		return ZW_ENOTFOUND;
	}
	*io = f->io;
	return 0;
}

int plat_open_pieces(const char *base, zw_io_t *io)
{
	char p[4200];
	host_files_t *f = malloc(sizeof(*f));
	if (!f)
		return ZW_ENOMEM;
	full(p, sizeof(p), base);
	if (host_files_open_pieces(f, p, 'P') < 0) {
		free(f);
		return ZW_ENOTFOUND;
	}
	*io = f->io;
	return 0;
}

void plat_close(zw_io_t *io)
{
	if (io->ctx) {
		host_files_close(io->ctx);
		free(io->ctx);
	}
	memset(io, 0, sizeof(*io));
}

int plat_load(const char *path, uint8_t **data, uint32_t *len)
{
	char p[4200];
	FILE *f;
	long n;
	full(p, sizeof(p), path);
	if (!(f = fopen(p, "rb")))
		return ZW_ENOTFOUND;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	*data = zw_alloc((size_t)n + 1);
	if (!*data || fread(*data, 1, (size_t)n, f) != (size_t)n) {
		fclose(f);
		zw_free(*data);
		*data = NULL;
		return ZW_EIO;
	}
	fclose(f);
	*len = (uint32_t)n;
	return 0;
}

int plat_load_max(const char *path, uint32_t max, uint8_t **data, uint32_t *len)
{
	int r = plat_load(path, data, len);
	if (r == 0 && *len > max)
		*len = max;
	return r;
}

int plat_save(const char *path, const void *data, uint32_t len)
{
	/* like the device: only into an existing file, never past its end */
	char p[4200];
	FILE *f;
	struct stat st;
	full(p, sizeof(p), path);
	if (stat(p, &st) != 0 || (uint64_t)st.st_size < len || !(f = fopen(p, "r+b")))
		return ZW_ENOTFOUND;
	if (fwrite(data, 1, len, f) != len) {
		fclose(f);
		return ZW_EIO;
	}
	return fclose(f) == 0 ? 0 : ZW_EIO;
}
