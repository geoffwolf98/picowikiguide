/*
 * PicoWikiGuide - PicoCalc keyboard: the STM32 on I2C1 (SDA 6, SCL 7,
 * address 0x1F)
 */
#ifndef PWG_KBD_H
#define PWG_KBD_H
#include <stdint.h>

/* raw codes from the keyboard controller */
#define KC_BS     0x08
#define KC_TAB    0x09
#define KC_ENTER  0x0A
#define KC_ALT    0xA1
#define KC_SHL    0xA2
#define KC_SHR    0xA3
#define KC_SYM    0xA4
#define KC_CTRL   0xA5
#define KC_ESC    0xB1
#define KC_LEFT   0xB4
#define KC_UP     0xB5
#define KC_DOWN   0xB6
#define KC_RIGHT  0xB7
#define KC_CAPS   0xC1
#define KC_BREAK  0xD0
#define KC_INS    0xD1
#define KC_HOME   0xD2
#define KC_DEL    0xD4
#define KC_END    0xD5
#define KC_PGUP   0xD6
#define KC_PGDN   0xD7
#define KC_F1     0x81
#define KC_F5     0x85
#define KC_F10    0x90
#define KC_POWER  0x91

enum { KS_IDLE, KS_PRESSED, KS_HOLD, KS_RELEASED };

void kbd_init(void);
/* one event from the controller's queue: 1 and the code and state, or 0 */
int kbd_read(int *code, int *state);
int kbd_read_raw(int *code, int *state, uint8_t *b0, uint8_t *b1);
int kbd_battery(void);		/* raw battery register, -1: failed */
void kbd_backlight(int v);
int kbd_backlight_get(void);	/* -1: failed */	/* screen backlight 0..255 */
/* ask the power chip to switch everything off after delay_s seconds
 * (at least 6; needs a recent keyboard firmware) */
void kbd_power_off(int delay_s);
#endif
