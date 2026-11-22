/*
 * PicoWikiGuide - what each platform (PicoCalc, PC window, PC tests)
 * provides to the app
 */
#ifndef PLAT_H
#define PLAT_H

#include "zw.h"

/* keys: printable ASCII as itself, the rest below */
enum {
	K_BS = 8, K_TAB = 9, K_ENTER = 13, K_ESC = 27, K_DEL = 127,
	K_UP = 0x100, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_END, K_PGUP, K_PGDN,
	K_F1, K_F2, K_F3, K_F4, K_F5, K_INS,
};
#define KM_SHIFT 1
#define KM_CTRL  2
#define KM_ALT   4

enum { PE_NONE, PE_KEY, PE_QUIT, PE_IDLE, PE_TICK };	/* PE_TICK: about once a second */

typedef struct plat_event {
	int type;
	int key;		/* PE_KEY */
	int mods;		/* KM_ flags */
} plat_event_t;

/* the screen: SCREEN_H rows of SCREEN_STRIDE bytes, 1 bit per pixel,
 * MSB leftmost, set = black */
void plat_present(const uint8_t *fb);

int plat_poll(plat_event_t *e);		/* 0: nothing waiting */
int plat_wait(plat_event_t *e);		/* blocks; PE_IDLE after a long wait */
int plat_keys_held(void);		/* a key held at start (safe mode) */
uint32_t plat_ms(void);
void plat_keepalive(void);		/* long work: feeds the watchdog */
void plat_power_off(void);
/* restart the device (state already saved); returns where there is
 * no such thing (the PC) */
void plat_restart(void);
/* ms until the idle switch-off, or -1 where there is none */
int32_t plat_off_in(void);
/* free memory in bytes, and the largest block that can be had; -1 where
 * not known */
int plat_mem(uint32_t *free_bytes, uint32_t *largest);
/* how the free memory lies: in holes between blocks in use, and at the
 * top of the heap (one piece); -1 where not known */
int plat_mem_split(uint32_t *holes, uint32_t *top);

/* storage.  Paths are relative to the card root, 8.3 names:
 * "ENPEDIA/ENPEDIA.TOC" */
/* folders in the card root that hold a wiki (FOLDER/FOLDER.TOC) */
int plat_list_wikis(char names[][16], int max);
/* open a file for reading at any offset */
int plat_open(const char *path, zw_io_t *io);
/* the pieces base.P00, base.P01, ... as one file (each piece but the
 * last is exactly 1 << shift bytes) */
/* base.P00, base.P01 ... (kind 'P'; 'I' for the pictures) as one */
int plat_open_pieces(const char *base, char kind, zw_io_t *io);
void plat_close(zw_io_t *io);
/* small files: read all (zw_alloc'd), or overwrite an existing file
 * from its start without changing its size (never creates a file) */
int plat_load(const char *path, uint8_t **data, uint32_t *len);
/* the same, at most max bytes (the start of the file) */
int plat_load_max(const char *path, uint32_t max, uint8_t **data, uint32_t *len);
int plat_save(const char *path, const void *data, uint32_t len);

#endif
