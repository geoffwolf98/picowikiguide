/*
 * PicoWikiGuide - the wiki's dictionary in the Pico's flash
 */
#ifndef DICTFLASH_H
#define DICTFLASH_H

#include "zw.h"

/* make the flash copy equal to the len byte dictionary at off in toc
 * (tmp: a buffer of a multiple of 4 KB to read it through; notice is
 * called before anything is written) and return where it can be read,
 * or NULL */
const uint8_t *dict_flash_place(const zw_io_t *toc, uint64_t off, uint32_t len,
				uint8_t *tmp, uint32_t tmp_len, void (*notice)(void));

#endif
