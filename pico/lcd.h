/*
 * PicoWikiGuide - PicoCalc LCD (ST7365P, ILI9488 compatible, 320 x 320)
 * on SPI1: SCK 10, MOSI 11, MISO 12, CS 13, DC 14, RST 15.
 * Pixels go out as 18 bit colour (3 bytes), as ClockworkPi's code does.
 * The frame buffer has 2 bits a pixel (4 greys).
 */
#ifndef PWG_LCD_H
#define PWG_LCD_H
#include <stdint.h>
void lcd_init(void);
/* show a 2 bpp frame buffer (80 bytes per row, MSB left, 0 white .. 3 black);
 * only rows that changed since the last call are sent */
void lcd_present(const uint8_t *fb);
/* send every row next time */
void lcd_invalidate(void);
/* panel on or off (the frame memory is kept) */
void lcd_display(int on);
#endif
