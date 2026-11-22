/*
 * PicoWikiGuide - PicoCalc LCD driver (init sequence from ClockworkPi's
 * picocalc examples, rewritten)
 */
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"
#include "lcd.h"
#include "present.h"

#define LCD_SPI   spi1
#define LCD_SCK   10
#define LCD_MOSI  11
#define LCD_MISO  12
#define LCD_CS    13
#define LCD_DC    14
#define LCD_RST   15
#define LCD_HZ    25000000
#define W         320
#define H         320
#define STRIDE    80	/* 2 bits a pixel */

static uint8_t shown[STRIDE * H];
static int valid;

static void cmd(uint8_t c)
{
	gpio_put(LCD_DC, 0);
	gpio_put(LCD_CS, 0);
	spi_write_blocking(LCD_SPI, &c, 1);
	gpio_put(LCD_CS, 1);
}

static void data(const uint8_t *d, size_t n)
{
	gpio_put(LCD_DC, 1);
	gpio_put(LCD_CS, 0);
	spi_write_blocking(LCD_SPI, d, n);
	gpio_put(LCD_CS, 1);
}

static void cmd_data(uint8_t c, const uint8_t *d, size_t n)
{
	cmd(c);
	if (n)
		data(d, n);
}

void lcd_init(void)
{
	static const uint8_t pgamma[] = { 0x00, 0x03, 0x09, 0x08, 0x16, 0x0A, 0x3F, 0x78,
					  0x4C, 0x09, 0x0A, 0x08, 0x16, 0x1A, 0x0F };
	static const uint8_t ngamma[] = { 0x00, 0x16, 0x19, 0x03, 0x0F, 0x05, 0x32, 0x45,
					  0x46, 0x04, 0x0E, 0x0D, 0x35, 0x37, 0x0F };
	gpio_init(LCD_CS);
	gpio_init(LCD_DC);
	gpio_init(LCD_RST);
	gpio_set_dir(LCD_CS, GPIO_OUT);
	gpio_set_dir(LCD_DC, GPIO_OUT);
	gpio_set_dir(LCD_RST, GPIO_OUT);
	gpio_put(LCD_CS, 1);
	spi_init(LCD_SPI, LCD_HZ);
	gpio_set_function(LCD_SCK, GPIO_FUNC_SPI);
	gpio_set_function(LCD_MOSI, GPIO_FUNC_SPI);
	gpio_set_function(LCD_MISO, GPIO_FUNC_SPI);

	gpio_put(LCD_RST, 1);
	sleep_ms(10);
	gpio_put(LCD_RST, 0);
	sleep_ms(10);
	gpio_put(LCD_RST, 1);
	sleep_ms(200);

	cmd_data(0xE0, pgamma, sizeof(pgamma));
	cmd_data(0xE1, ngamma, sizeof(ngamma));
	cmd_data(0xC0, (const uint8_t[]){ 0x17, 0x15 }, 2);	/* power control 1 */
	cmd_data(0xC1, (const uint8_t[]){ 0x41 }, 1);		/* power control 2 */
	cmd_data(0xC5, (const uint8_t[]){ 0x00, 0x12, 0x80 }, 3);	/* VCOM */
	cmd_data(0x36, (const uint8_t[]){ 0x48 }, 1);		/* MX, BGR */
	cmd_data(0x3A, (const uint8_t[]){ 0x66 }, 1);		/* 18 bit colour */
	cmd_data(0xB0, (const uint8_t[]){ 0x00 }, 1);
	cmd_data(0xB1, (const uint8_t[]){ 0xA0 }, 1);		/* frame rate */
	cmd(0x21);						/* inversion on (the panel needs it) */
	cmd_data(0xB4, (const uint8_t[]){ 0x02 }, 1);
	cmd_data(0xB6, (const uint8_t[]){ 0x02, 0x02, 0x3B }, 3);
	cmd_data(0xB7, (const uint8_t[]){ 0xC6 }, 1);
	cmd_data(0xE9, (const uint8_t[]){ 0x00 }, 1);
	cmd_data(0xF7, (const uint8_t[]){ 0xA9, 0x51, 0x2C, 0x82 }, 4);
	cmd(0x11);						/* sleep out */
	sleep_ms(120);
	cmd(0x29);						/* display on */
	sleep_ms(120);
	cmd_data(0x36, (const uint8_t[]){ 0x48 }, 1);
	valid = 0;
}

void lcd_invalidate(void)
{
	valid = 0;
}

/* set the window and start a memory write, all under one chip select
 * (as ClockworkPi's define_region_spi does); CS stays low for the pixels */
static void region(int y0, int y1)
{
	uint8_t c[4] = { 0, 0, (W - 1) >> 8, (W - 1) & 0xff }, k;
	gpio_put(LCD_CS, 0);
	gpio_put(LCD_DC, 0);
	k = 0x2A;
	spi_write_blocking(LCD_SPI, &k, 1);
	gpio_put(LCD_DC, 1);
	spi_write_blocking(LCD_SPI, c, 4);
	c[0] = (uint8_t)(y0 >> 8);
	c[1] = (uint8_t)y0;
	c[2] = (uint8_t)(y1 >> 8);
	c[3] = (uint8_t)y1;
	gpio_put(LCD_DC, 0);
	k = 0x2B;
	spi_write_blocking(LCD_SPI, &k, 1);
	gpio_put(LCD_DC, 1);
	spi_write_blocking(LCD_SPI, c, 4);
	gpio_put(LCD_DC, 0);
	k = 0x2C;
	spi_write_blocking(LCD_SPI, &k, 1);
	gpio_put(LCD_DC, 1);
}

static void spi_rows(const uint8_t *rgb)
{
	spi_write_blocking(LCD_SPI, rgb, W * 3);
}

static void spi_end(void)
{
	gpio_put(LCD_CS, 1);
}

void lcd_present(const uint8_t *fb)
{
	/* grey levels, a little darker than linear so text keeps its weight;
	 * 0 white .. 3 black (with inversion on, 0x00 shows as black); 3
	 * bytes a pixel, red, green, blue, the panel using the top 6 bits */
	static const uint8_t level[4] = { 0xFC, 0x9C, 0x4C, 0x00 };
	present_rows(fb, shown, &valid, level, region, spi_rows, spi_end);
}

void lcd_display(int on)
{
	cmd(on ? 0x29 : 0x28);	/* display on / off */
}
