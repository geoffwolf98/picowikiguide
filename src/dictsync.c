/*
 * PicoWikiGuide - keep a copy of a wiki's dictionary in flash
 */
#include "dictsync.h"

int dictsync(dictsync_t *d, uint32_t len)
{
	uint32_t pos, s, chunk = d->tmp_len - d->tmp_len % d->sector;
	int noticed = 0;

	d->written = 0;
	if (len > d->area_len || !chunk || d->area_len % d->sector)
		return ZW_ERANGE;
	for (pos = 0; pos < len; pos += chunk) {
		uint32_t n = len - pos < chunk ? len - pos : chunk;
		uint32_t padded = (n + d->sector - 1) / d->sector * d->sector;
		if (d->read(d->ctx, pos, d->tmp, n) < 0)
			return ZW_EIO;
		memset(d->tmp + n, 0xff, padded - n);
		for (s = 0; s < padded; s += d->sector) {
			const uint8_t *want = d->tmp + s;
			uint32_t off = pos + s;
			if (memcmp(d->area + off, want, d->sector) == 0)
				continue;
			if (!noticed && d->notice) {
				d->notice(d->ctx);
				noticed = 1;
			}
			if (d->erase(d->ctx, off, d->sector) < 0 ||
			    d->program(d->ctx, off, want, d->sector) < 0 ||
			    memcmp(d->area + off, want, d->sector) != 0)
				return ZW_EIO;
			d->written++;
		}
	}
	return ZW_OK;
}
