/*
 * PicoWikiGuide - small raw deflate decoder
 */
#ifndef INFLATE_H
#define INFLATE_H

#include "zw.h"

/* decompress raw deflate data into out; returns the bytes written, or
 * -1 for bad or truncated data (or more than outlen bytes) */
long zw_inflate(uint8_t *out, uint32_t outlen, const uint8_t *in, uint32_t inlen);

#endif
