/*
 * PicoWikiGuide - the wiki's dictionary in the Pico's flash
 *
 * The area is the last 112 KB of the flash the firmware can see.  The
 * chip maps flash for the program through QMI address translation
 * window 0: with BOOTSEL flashing that is all 4 MB from 0; under pelrun's
 * UF2 Loader it is the app partition (from 8 KB to the end).  Either way
 * the area is the same physical place, the end of the chip, well above
 * the firmware (about 1.3 MB).  It is written with the SDK's flash
 * functions (run from RAM, interrupts off, the XIP cache flushed after)
 * at the physical offset worked out from the window.
 */
#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/structs/qmi.h"
#include "dictflash.h"
#include "dictsync.h"
#include "zw.h"

#define AREA_LEN (112 * 1024)

extern char __flash_binary_end;

typedef struct {
	const zw_io_t *toc;
	uint64_t off;
	uint32_t phys;		/* physical flash offset of the area */
	void (*notice)(void);
} ctx_t;

static int fl_erase(void *c, uint32_t off, uint32_t len)
{
	ctx_t *x = c;
	uint32_t s = save_and_disable_interrupts();
	flash_range_erase(x->phys + off, len);
	restore_interrupts(s);
	return 0;
}

static int fl_program(void *c, uint32_t off, const uint8_t *buf, uint32_t len)
{
	ctx_t *x = c;
	uint32_t s = save_and_disable_interrupts();
	flash_range_program(x->phys + off, buf, len);
	restore_interrupts(s);
	return 0;
}

static int fl_read(void *c, uint32_t off, uint8_t *buf, uint32_t len)
{
	ctx_t *x = c;
	return zw_io_read(x->toc, x->off + off, buf, len);
}

static void fl_notice(void *c)
{
	ctx_t *x = c;
	if (x->notice)
		x->notice();
}

const uint8_t *dict_flash_place(const zw_io_t *toc, uint64_t off, uint32_t len,
				uint8_t *tmp, uint32_t tmp_len, void (*notice)(void))
{
	uint32_t a = qmi_hw->atrans[0];
	uint32_t base = ((a & QMI_ATRANS0_BASE_BITS) >> QMI_ATRANS0_BASE_LSB) * FLASH_SECTOR_SIZE;
	uint32_t vis = ((a & QMI_ATRANS0_SIZE_BITS) >> QMI_ATRANS0_SIZE_LSB) * FLASH_SECTOR_SIZE;
	uint32_t area_off;
	ctx_t x;
	dictsync_t d;

	if (base >= PICO_FLASH_SIZE_BYTES)
		return NULL;
	if (vis > PICO_FLASH_SIZE_BYTES - base)
		vis = PICO_FLASH_SIZE_BYTES - base;
	if (vis < AREA_LEN || len > AREA_LEN)
		return NULL;
	area_off = vis - AREA_LEN;
	/* never over the firmware (or the UF2 Loader, below base) */
	if ((uintptr_t)&__flash_binary_end > XIP_BASE + area_off ||
	    base + area_off + AREA_LEN > PICO_FLASH_SIZE_BYTES)
		return NULL;
	x.toc = toc;
	x.off = off;
	x.phys = base + area_off;
	x.notice = notice;
	memset(&d, 0, sizeof(d));
	d.area = (const uint8_t *)(XIP_BASE + area_off);
	d.area_len = AREA_LEN;
	d.sector = FLASH_SECTOR_SIZE;
	d.erase = fl_erase;
	d.program = fl_program;
	d.read = fl_read;
	d.notice = fl_notice;
	d.ctx = &x;
	d.tmp = tmp;
	d.tmp_len = tmp_len;
	if (dictsync(&d, len) != ZW_OK)
		return NULL;
	return d.area;
}
