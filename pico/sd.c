/*
 * PicoWikiGuide - SD card driver (SPI mode)
 */
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"
#include "sd.h"

#define SD_SPI   spi0
#define SD_SCK   18
#define SD_MOSI  19
#define SD_MISO  16
#define SD_CS    17
#define SD_FAST  25000000

static int sdhc;
static uint32_t hz;

static uint8_t xfer(uint8_t v)
{
	uint8_t r;
	spi_write_read_blocking(SD_SPI, &v, &r, 1);
	return r;
}

static void cs(int on)
{
	gpio_put(SD_CS, on ? 0 : 1);
	xfer(0xFF);	/* clocks so the card lets go of MISO */
}

static int wait_ready(uint32_t ms)
{
	absolute_time_t end = make_timeout_time_ms(ms);
	do {
		if (xfer(0xFF) == 0xFF)
			return 0;
	} while (absolute_time_diff_us(get_absolute_time(), end) > 0);
	return -1;
}

static uint8_t command(uint8_t cmd, uint32_t arg)
{
	uint8_t b[6], r = 0xFF;
	int i;
	if (cmd & 0x80) {	/* ACMD: CMD55 first */
		cmd &= 0x7f;
		r = command(55, 0);
		if (r > 1)
			return r;
	}
	cs(0);
	cs(1);
	if (cmd != 0 && wait_ready(500) < 0)
		return 0xFF;
	b[0] = (uint8_t)(0x40 | cmd);
	b[1] = (uint8_t)(arg >> 24);
	b[2] = (uint8_t)(arg >> 16);
	b[3] = (uint8_t)(arg >> 8);
	b[4] = (uint8_t)arg;
	b[5] = cmd == 0 ? 0x95 : cmd == 8 ? 0x87 : 0x01;
	spi_write_blocking(SD_SPI, b, 6);
	if (cmd == 12)
		xfer(0xFF);	/* stuff byte */
	for (i = 0; i < 10; ++i) {
		r = xfer(0xFF);
		if (!(r & 0x80))
			break;
	}
	return r;
}

int sd_init(void)
{
	uint8_t ocr[4];
	int i;
	absolute_time_t end;

	spi_init(SD_SPI, 400000);
	gpio_set_function(SD_SCK, GPIO_FUNC_SPI);
	gpio_set_function(SD_MOSI, GPIO_FUNC_SPI);
	gpio_set_function(SD_MISO, GPIO_FUNC_SPI);
	gpio_pull_up(SD_MISO);
	gpio_init(SD_CS);
	gpio_set_dir(SD_CS, GPIO_OUT);
	gpio_put(SD_CS, 1);
	for (i = 0; i < 10; ++i)
		xfer(0xFF);	/* 80 clocks with CS high */

	if (command(0, 0) != 1) {
		cs(0);
		return -1;	/* no card, or it does not answer */
	}
	sdhc = 0;
	if (command(8, 0x1AA) == 1) {
		for (i = 0; i < 4; ++i)
			ocr[i] = xfer(0xFF);
		if (ocr[2] != 0x01 || ocr[3] != 0xAA) {
			cs(0);
			return -2;
		}
		end = make_timeout_time_ms(1500);
		while (command(0x80 | 41, 0x40000000) != 0)
			if (absolute_time_diff_us(get_absolute_time(), end) <= 0) {
				cs(0);
				return -3;
			}
		if (command(58, 0) == 0) {
			for (i = 0; i < 4; ++i)
				ocr[i] = xfer(0xFF);
			sdhc = (ocr[0] & 0x40) != 0;
		}
	} else {
		end = make_timeout_time_ms(1500);
		while (command(0x80 | 41, 0) != 0)
			if (absolute_time_diff_us(get_absolute_time(), end) <= 0) {
				cs(0);
				return -3;
			}
		command(16, 512);
	}
	cs(0);
	hz = spi_set_baudrate(SD_SPI, SD_FAST);
	return 0;
}

uint32_t sd_hz(void) { return hz; }
int sd_is_sdhc(void) { return sdhc; }

static int read_block(uint8_t *buf)
{
	absolute_time_t end = make_timeout_time_ms(200);
	uint8_t t;
	do {
		t = xfer(0xFF);
		if (t == 0xFE)
			break;
	} while (absolute_time_diff_us(get_absolute_time(), end) > 0);
	if (t != 0xFE)
		return -1;
	spi_read_blocking(SD_SPI, 0xFF, buf, 512);
	xfer(0xFF);	/* CRC, not checked */
	xfer(0xFF);
	return 0;
}

int sd_read(uint32_t lba, void *buf, uint32_t count)
{
	uint8_t *p = buf;
	uint32_t addr = sdhc ? lba : lba * 512;
	if (!count)
		return 0;
	if (count == 1) {
		int r = command(17, addr) == 0 ? read_block(p) : -1;
		cs(0);
		return r;
	}
	if (command(18, addr) != 0) {
		cs(0);
		return -1;
	}
	while (count--) {
		if (read_block(p) < 0) {
			command(12, 0);
			cs(0);
			return -1;
		}
		p += 512;
	}
	command(12, 0);
	wait_ready(500);
	cs(0);
	return 0;
}

int sd_write(uint32_t lba, const void *buf)
{
	uint32_t addr = sdhc ? lba : lba * 512;
	uint8_t r;
	if (command(24, addr) != 0) {
		cs(0);
		return -1;
	}
	xfer(0xFF);
	xfer(0xFE);	/* start token */
	spi_write_blocking(SD_SPI, buf, 512);
	xfer(0xFF);	/* CRC */
	xfer(0xFF);
	r = xfer(0xFF);
	if ((r & 0x1F) != 0x05) {
		cs(0);
		return -2;
	}
	r = wait_ready(500);
	cs(0);
	return r ? -3 : 0;
}
