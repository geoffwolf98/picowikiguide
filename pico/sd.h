/*
 * PicoWikiGuide - SD card in SPI mode on SPI0: SCK 18, MOSI 19,
 * MISO 16, CS 17
 */
#ifndef PWG_SD_H
#define PWG_SD_H
#include <stdint.h>
int sd_init(void);			/* 0 = ok */
int sd_read(uint32_t lba, void *buf, uint32_t count);
int sd_write(uint32_t lba, const void *buf);	/* one sector */
uint32_t sd_hz(void);
int sd_is_sdhc(void);
#endif
