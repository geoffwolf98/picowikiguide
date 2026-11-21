/*
 * PicoWikiGuide - the application, driven by the platform's main loop
 */
#ifndef APP_H
#define APP_H

#include "zw.h"
#include "plat.h"

int app_run(void);			/* the whole program */
void app_init(void);			/* or step by step (tests) */
void app_event(const plat_event_t *e);
void app_key(int key, int mods);
void app_redraw(void);			/* draw the current screen again */
void app_describe(char *out, size_t cap);	/* tests */

#endif
