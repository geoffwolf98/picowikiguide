/*
 * PicoWikiGuide PC tests: the SD card driver's interface on a card image
 * file, so the PicoCalc's file code (pico/plat_storage.c) runs on the PC
 */
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include "sd.h"

static FILE *img;
uint32_t sd_image_reads, sd_image_writes;

int sd_image_open(const char *path)
{
	img = fopen(path, "r+b");
	return img ? 0 : -1;
}

int sd_init(void) { return img ? 0 : -1; }
uint32_t sd_hz(void) { return 0; }
int sd_is_sdhc(void) { return 1; }

int sd_read(uint32_t lba, void *buf, uint32_t count)
{
	sd_image_reads++;
	if (fseeko(img, (off_t)lba * 512, SEEK_SET) != 0 || fread(buf, 512, count, img) != count)
		return -1;
	return 0;
}

int sd_write(uint32_t lba, const void *buf)
{
	sd_image_writes++;
	if (fseeko(img, (off_t)lba * 512, SEEK_SET) != 0 || fwrite(buf, 512, 1, img) != 1)
		return -1;
	fflush(img);
	return 0;
}

int plat_sector_read(uint32_t lba, void *buf, uint32_t count) { return sd_read(lba, buf, count); }
