/*
 * PicoWikiGuide hardware test for the PicoCalc: screen, keyboard and SD
 * card, without the app.  Shows a test pattern, the SD card's details
 * and top folder, a read speed, the battery, and every key event.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "zw.h"
#include "font.h"
#include "gfx.h"
#include "fat.h"
#include "blockdev.h"
#include "lcd.h"
#include "overlay.h"
#include "kbd.h"
#include "sd.h"

/* what the shared code expects from a platform */
zw_stats_t zw_stats;
int zw_cancel;
void (*zw_progress)(int stage, uint32_t done, uint32_t total);
void *zw_alloc(size_t n) { return malloc(n ? n : 1); }
void *zw_calloc(size_t n) { return calloc(1, n ? n : 1); }
void zw_free(void *p) { free(p); }
void *zw_realloc(void *p, size_t o, size_t n) { (void)o; return realloc(p, n ? n : 1); }
void zw_log(const char *fmt, ...) { (void)fmt; }
int plat_sector_read(uint32_t lba, void *buf, uint32_t count) { return sd_read(lba, buf, count); }

static uint8_t fb[SCREEN_STRIDE * SCREEN_H];
static char sdline[64], fatline[64], speedline[64], names[8][40];
static int nnames;
static char events[7][48];
static int mods;
static char step[48] = "starting";
static uint32_t polls, kerr, beats;
static uint8_t raw0, raw1;

static void txt(int x, int base, int f, const char *s)
{
	font_draw(fb, SCREEN_STRIDE, SCREEN_H, x, base, f, s, strlen(s), BLACK);
}

static const char *key_name(int c)
{
	static char s[8];
	switch (c) {
	case 0x08: return "Backspace";
	case 0x09: return "Tab";
	case 0x0A: return "Enter";
	case 0x20: return "Space";
	case 0xA1: return "Alt";
	case 0xA2: return "Shift L";
	case 0xA3: return "Shift R";
	case 0xA4: return "Sym";
	case 0xA5: return "Ctrl";
	case 0xB1: return "Esc";
	case 0xB4: return "Left";
	case 0xB5: return "Up";
	case 0xB6: return "Down";
	case 0xB7: return "Right";
	case 0xC1: return "Caps Lock";
	case 0xD0: return "Break";
	case 0xD1: return "Insert";
	case 0xD2: return "Home";
	case 0xD4: return "Del";
	case 0xD5: return "End";
	case 0xD6: return "Page Up";
	case 0xD7: return "Page Down";
	case 0x91: return "Power";
	}
	if (c >= 0x81 && c <= 0x89) {
		snprintf(s, sizeof(s), "F%d", c - 0x80);
		return s;
	}
	if (c == 0x90)
		return "F10";
	if (c > 32 && c < 127) {
		s[0] = '\'';
		s[1] = (char)c;
		s[2] = '\'';
		s[3] = 0;
		return s;
	}
	return "?";
}

static void draw(void)
{
	int i, y;
	char s[64];
	memset(fb, 0, sizeof(fb));
	/* top bar and frame */
	gfx_fill(fb, SCREEN_STRIDE, SCREEN_H, 0, 0, SCREEN_W, 18, BLACK);
	font_draw(fb, SCREEN_STRIDE, SCREEN_H, 4, 13, FONT_SMALLL, "PicoWikiGuide hardware test " PWG_VERSION,
		  strlen("PicoWikiGuide hardware test " PWG_VERSION), WHITE);
	gfx_rect(fb, SCREEN_STRIDE, SCREEN_H, 0, 0, SCREEN_W, SCREEN_H, BLACK);
	/* pattern: a checkerboard and one-pixel lines (corners must show) */
	for (y = 0; y < 24; ++y)
		for (i = 0; i < 48; ++i)
			if (((i / 4) + (y / 4)) & 1)
				gfx_fill(fb, SCREEN_STRIDE, SCREEN_H, SCREEN_W - 52 + i, 24 + y, 1, 1, BLACK);
	for (i = 0; i < 12; ++i)
		gfx_hline(fb, SCREEN_STRIDE, SCREEN_H, SCREEN_W - 52, 52 + i * 2, 48, BLACK);

	y = 38;
	txt(6, y, FONT_TEXTL, sdline);
	y += 20;
	txt(6, y, FONT_SMALLL, fatline);
	y += 16;
	for (i = 0; i < nnames; ++i) {
		txt(14 + (i & 1) * 140, y + (i / 2) * 15, FONT_SMALLL, names[i]);
	}
	y += 4 * 15;
	txt(6, y, FONT_SMALLL, speedline);
	y += 16;
	snprintf(s, sizeof(s), "Battery register: %d", kbd_battery());
	txt(6, y, FONT_SMALLL, s);
	/* colour bars (shown over the frame buffer, as pictures are): top
	 * down red, green, blue; if red and blue are swapped, say so */
	{
		static uint8_t bars[48 * 36];
		int k;
		for (k = 0; k < 48 * 36; ++k)
			bars[k] = k < 48 * 12 ? 0xe0 : k < 48 * 24 ? 0x1c : 0x03;
		ov_clear();
		ov_add(SCREEN_W - 52, 70, 48, 36, bars, 1);
		txt(SCREEN_W - 120, 80, FONT_SMALLL, "red");
		txt(SCREEN_W - 120, 92, FONT_SMALLL, "green");
		txt(SCREEN_W - 120, 104, FONT_SMALLL, "blue");
	}
	y += 10;
	gfx_hline(fb, SCREEN_STRIDE, SCREEN_H, 0, y, SCREEN_W, BLACK);
	y += 18;
	snprintf(s, sizeof(s), "Keys (held: %s%s%s%s)", mods & 1 ? "Shift " : "", mods & 2 ? "Ctrl " : "",
		 mods & 4 ? "Alt " : "", mods & 8 ? "Sym" : "");
	txt(6, y, FONT_TEXTL, s);
	y += 4;
	for (i = 0; i < 7; ++i)
		txt(10, y + 15 + i * 15, FONT_SMALLL, events[i]);
	/* bottom line: what it is doing, a heartbeat and the keyboard's raw state */
	gfx_hline(fb, SCREEN_STRIDE, SCREEN_H, 0, SCREEN_H - 18, SCREEN_W, BLACK);
	snprintf(s, sizeof(s), "%s  %lu  kbd %lu/%lu %02X %02X", step, (unsigned long)beats,
		 (unsigned long)polls, (unsigned long)kerr, raw0, raw1);
	txt(4, SCREEN_H - 5, FONT_SMALLL, s);
	lcd_present(fb);
}

static int list_cb(void *arg, const fat_file_t *f)
{
	(void)arg;
	if (nnames < 8 && f->name[0] != '.') {
		snprintf(names[nnames++], sizeof(names[0]), "%s%s", f->name, (f->attr & 0x10) ? "/" : "");
	}
	return 0;
}

int main(void)
{
	fat_vol_t vol;
	int r;
	memset(&vol, 0, sizeof(vol));
	lcd_init();
	kbd_init();
	strcpy(sdline, "SD card: starting...");
	draw();

	strcpy(step, "sd_init");
	draw();
	r = sd_init();
	if (r < 0) {
		snprintf(sdline, sizeof(sdline), "SD card: not found (error %d)", r);
	} else {
		snprintf(sdline, sizeof(sdline), "SD card: ok, %s, %lu kHz", sd_is_sdhc() ? "SDHC/SDXC" : "SDSC",
			 (unsigned long)(sd_hz() / 1000));
		strcpy(step, "fat_mount");
		draw();
		bd_init(16);
		if (fat_mount(&vol) < 0) {
			strcpy(fatline, "FAT: no FAT16/FAT32 file system found");
		} else {
			snprintf(fatline, sizeof(fatline), "%s, %lu clusters of %u KB; top folder:",
				 vol.fat32 ? "FAT32" : "FAT16", (unsigned long)vol.cluster_count,
				 (1u << vol.spc_shift) / 2);
			strcpy(step, "fat_list");
			draw();
			fat_list(&vol, "", list_cb, NULL);
			if (!nnames)
				fat_list(&vol, "/", list_cb, NULL);
		}
		strcpy(step, "speed test");
		draw();
		{
			/* read 2 MB in 64-sector reads, from the start of the data area */
			static uint8_t buf[64 * 512];
			uint32_t lba = vol.data_lba, i, t0 = to_ms_since_boot(get_absolute_time()), ms;
			int bad = 0;
			for (i = 0; i < 64; ++i)
				if (sd_read(lba + i * 64, buf, 64) < 0)
					bad++;
			ms = to_ms_since_boot(get_absolute_time()) - t0;
			snprintf(speedline, sizeof(speedline), "Read speed: %lu KB/s%s",
				 (unsigned long)(2048u * 1000u / (ms ? ms : 1)), bad ? " (read errors!)" : "");
		}
	}
	strcpy(step, "ready");
	draw();

	for (;;) {
		int code, state, got;
		static uint32_t last;
		uint32_t now = to_ms_since_boot(get_absolute_time());
		got = kbd_read_raw(&code, &state, &raw0, &raw1);
		polls++;
		if (got < 0)
			kerr++;
		if (now - last >= 500) {
			last = now;
			beats++;
			draw();
		}
		if (got > 0) {
			static const char *st[] = { "idle", "pressed", "held", "released" };
			int bit = code == 0xA2 || code == 0xA3 ? 1 : code == 0xA5 ? 2 : code == 0xA1 ? 4 :
				  code == 0xA4 ? 8 : 0;
			if (bit) {
				if (state == KS_PRESSED || state == KS_HOLD)
					mods |= bit;
				else if (state == KS_RELEASED)
					mods &= ~bit;
			}
			memmove(events[1], events[0], sizeof(events[0]) * 6);
			snprintf(events[0], sizeof(events[0]), "0x%02X  %-10s %s", code, key_name(code),
				 state < 4 ? st[state] : "?");
			draw();
		}
	}
}
