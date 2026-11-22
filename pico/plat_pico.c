/*
 * PicoWikiGuide - the PicoCalc platform: screen, keyboard, SD card
 * (read-only FAT32, plus overwriting existing small files in place)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <unistd.h>
#include "pico/stdlib.h"
#include "hardware/watchdog.h"
#include "zw.h"
#include "plat.h"
#include "app.h"
#include "gfx.h"
#include "font.h"
#include "fat.h"
#include "blockdev.h"
#include "card.h"
#include "lcd.h"
#include "kbd.h"
#include "sd.h"
#include "plat_storage.h"
#include "splash.h"

#define IDLE_MS     60000		/* state saved after a minute without keys */
#define OFF_MS      (5 * 60000)	/* switched off after 5 minutes */
#define POLL_MS     10
#define MAX_PIECES  100

/* ------------------------------------------------------------------ */
/* what the shared code expects */

zw_stats_t zw_stats;
int zw_cancel;
void (*zw_progress)(int stage, uint32_t done, uint32_t total);
void *zw_alloc(size_t n) { return malloc(n ? n : 1); }
void *zw_calloc(size_t n) { return calloc(1, n ? n : 1); }
void zw_free(void *p) { free(p); }
void *zw_realloc(void *p, size_t o, size_t n) { (void)o; return realloc(p, n ? n : 1); }
void zw_log(const char *fmt, ...) { (void)fmt; }
int plat_sector_read(uint32_t lba, void *buf, uint32_t count) { return sd_read(lba, buf, count); }

/* ------------------------------------------------------------------ */
/* screen, time */

void plat_present(const uint8_t *fb) { lcd_present(fb); }
uint32_t plat_ms(void) { return to_ms_since_boot(get_absolute_time()); }
void plat_keepalive(void) {}
void plat_power_off(void) { kbd_power_off(6); }

static uint32_t last_key;

int32_t plat_off_in(void)
{
	uint32_t idle = plat_ms() - last_key;
	return idle >= OFF_MS ? 0 : (int32_t)(OFF_MS - idle);
}

/* the heap runs from the end of .bss up to __StackLimit */
extern char __StackLimit;
int plat_mem(uint32_t *free_bytes, uint32_t *largest)
{
	struct mallinfo m = mallinfo();
	char *top = sbrk(0);
	uint32_t tail = top && top != (char *)-1 && top < &__StackLimit ?
			(uint32_t)(&__StackLimit - top) : 0;
	uint32_t lo = 0, hi = (uint32_t)m.fordblks + tail;
	*free_bytes = hi;
	/* the largest block: try sizes (malloc only says yes or no) */
	while (hi - lo > 256) {
		uint32_t mid = lo + (hi - lo) / 2;
		void *p = malloc(mid);
		if (p) {
			free(p);
			lo = mid;
		} else {
			hi = mid;
		}
	}
	*largest = lo;
	return 0;
}

int plat_mem_split(uint32_t *holes, uint32_t *top)
{
	struct mallinfo m = mallinfo();
	char *end = sbrk(0);
	uint32_t tail = end && end != (char *)-1 && end < &__StackLimit ?
			(uint32_t)(&__StackLimit - end) : 0;
	/* fordblks: all free chunks including the top one; keepcost: the
	 * top one */
	*holes = (uint32_t)(m.fordblks - m.keepcost);
	*top = (uint32_t)m.keepcost + tail;
	return 0;
}

/* a restart asked for by the app (another wiki): no 2 second splash */
#define RESTART_MAGIC 0x50574752u	/* "PWGR" */
void plat_restart(void)
{
	watchdog_hw->scratch[0] = RESTART_MAGIC;
	watchdog_reboot(0, 0, 10);
	for (;;)
		tight_loop_contents();
}

/* ------------------------------------------------------------------ */
/* keyboard */

static int mods;	/* KM_ flags from the modifier keys */
static int held_at_start;
static uint32_t last_key;

static int map_code(int c)
{
	switch (c) {
	case KC_ENTER: return K_ENTER;
	case KC_BS: return K_BS;
	case KC_TAB: return K_TAB;
	case KC_ESC: return K_ESC;
	case KC_UP: return K_UP;
	case KC_DOWN: return K_DOWN;
	case KC_LEFT: return K_LEFT;
	case KC_RIGHT: return K_RIGHT;
	case KC_HOME: return K_HOME;
	case KC_END: return K_END;
	case KC_PGUP: return K_PGUP;
	case KC_PGDN: return K_PGDN;
	case KC_DEL: return K_DEL;
	case KC_INS: return K_INS;
	case KC_BREAK: return K_ESC;
	}
	if (c >= KC_F1 && c <= KC_F5)
		return K_F1 + (c - KC_F1);
	if (c >= 32 && c < 127)
		return c;
	return -1;
}

/* keys that repeat while held */
static int repeats(int k)
{
	return k == K_UP || k == K_DOWN || k == K_LEFT || k == K_RIGHT || k == K_PGUP ||
	       k == K_PGDN || k == K_BS;
}

int plat_poll(plat_event_t *e)
{
	int code, state, k, bit;
	while (kbd_read(&code, &state)) {
		bit = (code == KC_SHL || code == KC_SHR) ? KM_SHIFT : code == KC_CTRL ? KM_CTRL :
		      code == KC_ALT ? KM_ALT : 0;
		if (bit) {
			if (state == KS_PRESSED || state == KS_HOLD)
				mods |= bit;
			else if (state == KS_RELEASED)
				mods &= ~bit;
			continue;
		}
		k = map_code(code);
		if (k < 0)
			continue;
		if (state == KS_PRESSED || (state == KS_HOLD && repeats(k))) {
			e->type = PE_KEY;
			e->key = k;
			e->mods = mods;
			last_key = plat_ms();
			return 1;
		}
	}
	e->type = PE_NONE;
	return 0;
}

static void screen_message(const char *a, const char *b);

/* nothing pressed for OFF_MS: switch off through the power chip.  An
 * older keyboard firmware cannot do that; then the screen and its
 * backlight go off until a key is pressed. */
static void auto_off(void)
{
	uint32_t t0;
	int code, state, bl = kbd_backlight_get();
	screen_message("Switched off to save the battery.", "Press the power button to start again.");
	kbd_power_off(6);
	t0 = plat_ms();
	while (plat_ms() - t0 < 12000)		/* power goes in about 6 s */
		sleep_ms(100);
	/* still here: no power-off support */
	kbd_backlight(0);
	lcd_display(0);
	for (;;) {
		if (kbd_read(&code, &state) && state == KS_PRESSED)
			break;
		sleep_ms(50);
	}
	lcd_display(1);
	if (bl > 0)
		kbd_backlight(bl);
	else
		kbd_backlight(255);
	lcd_invalidate();
	app_redraw();
}

int plat_wait(plat_event_t *e)
{
	static uint32_t idle_sent, last_tick;
	for (;;) {
		uint32_t now;
		if (plat_poll(e)) {
			idle_sent = 0;
			return 1;
		}
		now = plat_ms();
		if (now - last_tick >= 1000) {
			last_tick = now;
			e->type = PE_TICK;
			return 1;
		}
		if (now - last_key > OFF_MS) {
			auto_off();
			last_key = plat_ms();
			idle_sent = 0;
			continue;
		}
		if (now - last_key > IDLE_MS && !idle_sent) {
			idle_sent = 1;	/* the app saves its state once */
			e->type = PE_IDLE;
			return 1;
		}
		sleep_ms(POLL_MS);
	}
}

int plat_keys_held(void)
{
	return held_at_start;
}

/* ------------------------------------------------------------------ */

/* messages and the splash draw into the app's frame buffer (no second
 * 25 KB buffer; the app draws its whole screen again afterwards) */
#define FB_SIZE (SCREEN_STRIDE * SCREEN_H)

static void screen_message(const char *a, const char *b)
{
	uint8_t *fb = app_framebuffer();
	memset(fb, 0, FB_SIZE);
	gfx_fill(fb, SCREEN_STRIDE, SCREEN_H, 0, 0, SCREEN_W, TOPBAR_H, BLACK);
	font_draw(fb, SCREEN_STRIDE, SCREEN_H, 4, 13, FONT_SMALLL, "PicoWikiGuide " PWG_VERSION,
		  strlen("PicoWikiGuide " PWG_VERSION), WHITE);
	font_draw(fb, SCREEN_STRIDE, SCREEN_H, 6, 60, FONT_TEXTL, a, strlen(a), BLACK);
	if (b)
		font_draw(fb, SCREEN_STRIDE, SCREEN_H, 6, 84, FONT_SMALLL, b, strlen(b), BLACK);
	lcd_present(fb);
}

extern const uint8_t wikifont_data[], wikifont_end[];

int main(void)
{
	uint32_t t0;
	int code, state;
	uint32_t shown;
	int quick = watchdog_caused_reboot() && watchdog_hw->scratch[0] == RESTART_MAGIC;
	watchdog_hw->scratch[0] = 0;
	lcd_init();
	if (quick) {
		screen_message("Opening the wiki...", NULL);
	} else {
		splash_draw(app_framebuffer());	/* DON'T PANIC, as early as possible */
		lcd_present(app_framebuffer());
	}
	shown = plat_ms();
	kbd_init();
	/* the Unicode fallback font, in flash */
	font_set_fallback(wikifont_data, (uint32_t)(wikifont_end - wikifont_data));
	/* a key held now: safe mode (do not reopen the last page) */
	t0 = plat_ms();
	while (!quick && plat_ms() - t0 < 300)
		if (kbd_read(&code, &state) && (state == KS_PRESSED || state == KS_HOLD) &&
		    code != KC_SHL && code != KC_SHR)
			held_at_start = 1;
	for (;;) {
		int r = sd_init();
		if (r == 0) {
			bd_init(8);
			if (storage_mount() == 0)
				break;
			screen_message("No FAT32 file system on the card.", "Press a key to try again.");
		} else {
			screen_message("No SD card, or it does not answer.", "Press a key to try again.");
		}
		while (!kbd_read(&code, &state) || state != KS_PRESSED)
			sleep_ms(20);
	}
	/* leave the start-up screen up for at least 2 seconds */
	while (!quick && plat_ms() - shown < 2000)
		sleep_ms(20);
	last_key = plat_ms();
	app_run();
	return 0;
}
