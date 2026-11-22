/*
 * pwg-sdl [--scale N] [--safe] CARD
 *
 * The app in a window, with the PC keyboard as the PicoCalc's.  CARD is
 * a folder laid out like the SD card (wiki folders and PWGSTATE.DAT).
 * F12 saves a screenshot (pwg-shot-N.png).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>
#include "app.h"
#include "gfx.h"
#include "plat_files.h"
#include "plat_common.h"
#include "overlay.h"
#include "splash.h"

static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *tex;
static uint8_t last[SCREEN_STRIDE * SCREEN_H];
static int safe;

void plat_present(const uint8_t *fb)
{
	static uint32_t px[SCREEN_W * SCREEN_H];
	static const uint8_t level[4] = { 0xf4, 0xa8, 0x5c, 0x10 };
	uint8_t rgb[SCREEN_W * 3];
	int y, x;
	memcpy(last, fb, sizeof(last));
	for (y = 0; y < SCREEN_H; ++y) {
		ov_row_rgb(fb + y * SCREEN_STRIDE, y, level, rgb);	/* with colour pictures */
		for (x = 0; x < SCREEN_W; ++x)
			px[y * SCREEN_W + x] = 0xff000000u | (uint32_t)rgb[x * 3] << 16 |
					       (uint32_t)rgb[x * 3 + 1] << 8 | rgb[x * 3 + 2];
	}
	SDL_UpdateTexture(tex, NULL, px, SCREEN_W * 4);
	SDL_RenderClear(ren);
	SDL_RenderCopy(ren, tex, NULL, NULL);
	SDL_RenderPresent(ren);
}

int plat_keys_held(void) { return safe; }
int32_t plat_off_in(void) { return -1; }
void plat_power_off(void) {}

static int map_key(SDL_Keycode k)
{
	switch (k) {
	case SDLK_UP: return K_UP;
	case SDLK_DOWN: return K_DOWN;
	case SDLK_LEFT: return K_LEFT;
	case SDLK_RIGHT: return K_RIGHT;
	case SDLK_HOME: return K_HOME;
	case SDLK_END: return K_END;
	case SDLK_PAGEUP: return K_PGUP;
	case SDLK_PAGEDOWN: return K_PGDN;
	case SDLK_RETURN: case SDLK_KP_ENTER: return K_ENTER;
	case SDLK_ESCAPE: return K_ESC;
	case SDLK_BACKSPACE: return K_BS;
	case SDLK_TAB: return K_TAB;
	case SDLK_DELETE: return K_DEL;
	case SDLK_INSERT: return K_INS;
	case SDLK_F1: return K_F1;
	case SDLK_F2: return K_F2;
	case SDLK_F3: return K_F3;
	case SDLK_F4: return K_F4;
	case SDLK_F5: return K_F5;
	}
	return -1;
}

static int convert(const SDL_Event *s, plat_event_t *e)
{
	e->type = PE_NONE;
	e->mods = 0;
	switch (s->type) {
	case SDL_QUIT:
		e->type = PE_QUIT;
		return 1;
	case SDL_WINDOWEVENT:
		if (s->window.event == SDL_WINDOWEVENT_EXPOSED)
			plat_present(last);
		return 0;
	case SDL_TEXTINPUT:
		/* printable characters (shift and layout already applied) */
		if ((unsigned char)s->text.text[0] >= 32 && (unsigned char)s->text.text[0] < 127 &&
		    !s->text.text[1]) {
			e->type = PE_KEY;
			e->key = (unsigned char)s->text.text[0];
			return 1;
		}
		return 0;
	case SDL_KEYDOWN: {
		int k = map_key(s->key.keysym.sym);
		SDL_Keymod m = s->key.keysym.mod;
		if (s->key.keysym.sym == SDLK_F12) {
			static int n;
			char name[64];
			snprintf(name, sizeof(name), "pwg-shot-%d.png", ++n);
			host_save_pbm(name, last);
			fprintf(stderr, "saved %s\n", name);
			return 0;
		}
		if (k < 0)
			return 0;
		e->type = PE_KEY;
		e->key = k;
		if (m & KMOD_SHIFT)
			e->mods |= KM_SHIFT;
		if (m & KMOD_CTRL)
			e->mods |= KM_CTRL;
		if (m & KMOD_ALT)
			e->mods |= KM_ALT;
		return 1;
	}
	}
	return 0;
}

int plat_poll(plat_event_t *e)
{
	SDL_Event s;
	while (SDL_PollEvent(&s))
		if (convert(&s, e))
			return 1;
	return 0;
}

int plat_wait(plat_event_t *e)
{
	static uint32_t last_key, idle_sent;
	SDL_Event s;
	if (!last_key)
		last_key = plat_ms();
	if (!SDL_WaitEventTimeout(&s, 1000)) {
		if (plat_ms() - last_key > 60000 && !idle_sent) {
			idle_sent = 1;
			e->type = PE_IDLE;
		} else {
			e->type = PE_TICK;
		}
		return 1;
	}
	if (convert(&s, e)) {
		if (e->type == PE_KEY) {
			last_key = plat_ms();
			idle_sent = 0;
		}
		return 1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	const char *card = NULL;
	int scale = 2, a;
	for (a = 1; a < argc; ++a) {
		if (strcmp(argv[a], "--scale") == 0 && a + 1 < argc)
			scale = atoi(argv[++a]);
		else if (strcmp(argv[a], "--safe") == 0)
			safe = 1;
		else
			card = argv[a];
	}
	if (!card) {
		fprintf(stderr, "usage: pwg-sdl [--scale N] [--safe] CARD-FOLDER\n");
		return 2;
	}
	if (host_load_wikifont(argv[0]) < 0)
		return 1;
	if (scale < 1)
		scale = 1;
	plat_files_set_card(card);
	if (SDL_Init(SDL_INIT_VIDEO) != 0) {
		fprintf(stderr, "SDL: %s\n", SDL_GetError());
		return 1;
	}
	win = SDL_CreateWindow("PicoWikiGuide", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
			       SCREEN_W * scale, SCREEN_H * scale, 0);
	ren = SDL_CreateRenderer(win, -1, 0);
	if (!win || !ren) {
		fprintf(stderr, "SDL: %s\n", SDL_GetError());
		return 1;
	}
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
	tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
				SCREEN_W, SCREEN_H);
	SDL_StartTextInput();
	{
		/* the start-up screen, as on the PicoCalc */
		static uint8_t fb[SCREEN_STRIDE * SCREEN_H];
		splash_draw(fb);
		plat_present(fb);
		SDL_Delay(1500);
		SDL_PumpEvents();
	}
	app_run();
	SDL_Quit();
	return 0;
}
