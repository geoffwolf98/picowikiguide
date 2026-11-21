/*
 * PicoWikiGuide - the start-up screen
 */
#ifndef SPLASH_H
#define SPLASH_H
#include "zw.h"
extern const int splash_w, splash_h;
extern const uint8_t splash_bits[];
/* draw the start-up screen into a frame buffer */
void splash_draw(uint8_t *fb);
#endif
