/*
 * pwg-headless CARD SCRIPT    run the app with keys from a script
 *
 * Script lines:
 *   key NAME [NAME...]   press keys: up down left right enter esc bs tab
 *                        del home end pgup pgdn f1..f5 space, a single
 *                        character, with shift+ ctrl+ alt+ prefixes
 *   type TEXT            type the text
 *   expect TEXT          the screen description must contain TEXT
 *   expectnot TEXT       ... must not contain TEXT
 *   describe             print the screen description
 *   shot FILE.pbm        save the screen
 *   restart              start the app again (state is kept on the card)
 *   idle                 a minute without keys (the app saves its state)
 *   mem                  print the free memory and the largest free block
 *                        (PWG_HEAP=BYTES: a heap like the PicoCalc's)
 *   memmark              remember the free memory now
 *   expectmem same       ... it must be the same now (no leak)
 *   expectmem largest N  the largest free block must be N bytes or more
 *   expectmem whole      all the free memory in one block (no holes)
 *   offin MS             a second goes by, MS before the idle switch-off
 *                        (-1: none)
 *   # comment
 * PWG_HELD=1 in the environment: a key is held at start (safe mode).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "gfx.h"
#include "plat_files.h"
#include "plat_common.h"
#ifdef CARD_IMAGE
#include "blockdev.h"
#include "plat_storage.h"
int sd_image_open(const char *path);
#endif

static uint8_t screen[SCREEN_STRIDE * SCREEN_H];

void plat_present(const uint8_t *fb) { memcpy(screen, fb, sizeof(screen)); }
int plat_poll(plat_event_t *e) { e->type = PE_NONE; return 0; }
int plat_wait(plat_event_t *e) { e->type = PE_QUIT; return 1; }
int plat_keys_held(void) { return getenv("PWG_HELD") != NULL; }
void plat_power_off(void) {}
static int32_t off_in = -1;
int32_t plat_off_in(void) { return off_in; }

static void event(int type)
{
	plat_event_t e;
	memset(&e, 0, sizeof(e));
	e.type = type;
	app_event(&e);
}

static void press(int key, int mods)
{
	plat_event_t e;
	e.type = PE_KEY;
	e.key = key;
	e.mods = mods;
	app_event(&e);
}

int main(int argc, char **argv)
{
	FILE *f;
	char line[1024], d[512];
	int lineno = 0, fails = 0;
	uint32_t mark = 0;
	if (argc < 3) {
		fprintf(stderr, "usage: pwg-headless CARD SCRIPT\n");
		return 2;
	}
#ifdef CARD_IMAGE
	/* the PicoCalc's file code on a card image */
	if (sd_image_open(argv[1]) < 0 || bd_init(8) < 0 || storage_mount() < 0) {
		fprintf(stderr, "cannot mount the card image %s\n", argv[1]);
		return 2;
	}
#else
	plat_files_set_card(argv[1]);
#endif
	if (!(f = fopen(argv[2], "r")))
		return 2;
	if (host_load_wikifont(argv[0]) < 0)
		return 2;
	app_init();
	while (fgets(line, sizeof(line), f)) {
		char *p = line, *arg;
		lineno++;
		line[strcspn(line, "\r\n")] = 0;
		while (*p == ' ')
			p++;
		if (!*p || *p == '#')
			continue;
		arg = strchr(p, ' ');
		if (arg)
			*arg++ = 0;
		else
			arg = p + strlen(p);
		if (strcmp(p, "key") == 0) {
			char *t = strtok(arg, " ");
			while (t) {
				int mods, k = host_key_by_name(t, &mods);
				if (k < 0) {
					fprintf(stderr, "line %d: unknown key %s\n", lineno, t);
					return 2;
				}
				press(k, mods);
				t = strtok(NULL, " ");
			}
		} else if (strcmp(p, "type") == 0) {
			for (; *arg; ++arg)
				press((unsigned char)*arg, 0);
		} else if (strcmp(p, "expect") == 0 || strcmp(p, "expectnot") == 0) {
			int want = strcmp(p, "expect") == 0;
			app_describe(d, sizeof(d));
			if ((strstr(d, arg) != NULL) != want) {
				printf("FAIL line %d: %s \"%s\"\n  screen: %s\n", lineno, p, arg, d);
				fails++;
			}
		} else if (strcmp(p, "describe") == 0) {
			app_describe(d, sizeof(d));
			printf("%s\n", d);
		} else if (strcmp(p, "shot") == 0) {
			host_save_pbm(arg, screen);
		} else if (strcmp(p, "restart") == 0) {
			app_init();
		} else if (strcmp(p, "mem") == 0) {
			uint32_t fr, big;
			if (plat_mem(&fr, &big) == 0)
				printf("memory free %u, largest %u\n", fr, big);
		} else if (strcmp(p, "memmark") == 0) {
			uint32_t big;
			plat_mem(&mark, &big);
		} else if (strcmp(p, "expectmem") == 0) {
			uint32_t fr = 0, big = 0;
			int ok = 1;
			if (plat_mem(&fr, &big) != 0) {
				/* no heap emulation: nothing to check */
			} else if (strcmp(arg, "same") == 0) {
				ok = fr == mark;
			} else if (strncmp(arg, "largest ", 8) == 0) {
				ok = big >= (uint32_t)atol(arg + 8);
			} else if (strcmp(arg, "whole") == 0) {
				ok = big + 8 >= fr;
			}
			if (!ok) {
				printf("FAIL line %d: expectmem %s: free %u (mark %u), largest %u\n",
				       lineno, arg, fr, mark, big);
				fails++;
			}
		} else if (strcmp(p, "idle") == 0) {
			event(PE_IDLE);
		} else if (strcmp(p, "offin") == 0) {
			off_in = atoi(arg);
			event(PE_TICK);
		} else {
			fprintf(stderr, "line %d: unknown command %s\n", lineno, p);
			return 2;
		}
	}
	fclose(f);
	if (fails)
		printf("%d failure(s)\n", fails);
	return fails != 0;
}
