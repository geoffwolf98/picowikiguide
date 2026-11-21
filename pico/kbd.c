/*
 * PicoWikiGuide - PicoCalc keyboard controller (register layout from
 * ClockworkPi's picocalc_keyboard firmware)
 */
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"
#include "kbd.h"

#define KBD_I2C   i2c1
#define KBD_SDA   6
#define KBD_SCL   7
#define KBD_ADDR  0x1F
#define KBD_HZ    10000		/* as ClockworkPi's examples */

#define REG_BKL   0x05
#define REG_FIF   0x09
#define REG_BAT   0x0b
#define REG_OFF   0x0e
#define WRITE     0x80

void kbd_init(void)
{
	i2c_init(KBD_I2C, KBD_HZ);
	gpio_set_function(KBD_SDA, GPIO_FUNC_I2C);
	gpio_set_function(KBD_SCL, GPIO_FUNC_I2C);
	gpio_pull_up(KBD_SDA);
	gpio_pull_up(KBD_SCL);
}

static int reg_read(uint8_t reg, uint8_t out[2])
{
	if (i2c_write_timeout_us(KBD_I2C, KBD_ADDR, &reg, 1, false, 100000) != 1)
		return -1;
	sleep_ms(16);	/* as ClockworkPi's examples */
	if (i2c_read_timeout_us(KBD_I2C, KBD_ADDR, out, 2, false, 100000) != 2)
		return -1;
	return 0;
}

static int reg_write(uint8_t reg, uint8_t v)
{
	uint8_t b[2] = { (uint8_t)(reg | WRITE), v };
	return i2c_write_timeout_us(KBD_I2C, KBD_ADDR, b, 2, false, 100000) == 2 ? 0 : -1;
}

int kbd_read(int *code, int *state)
{
	uint8_t b[2];
	if (reg_read(REG_FIF, b) < 0 || (b[0] == 0 && b[1] == 0))
		return 0;
	*state = b[0];
	*code = b[1];
	return 1;
}

int kbd_battery(void)
{
	uint8_t b[2];
	if (reg_read(REG_BAT, b) < 0)
		return -1;
	return b[1];
}

int kbd_backlight_get(void)
{
	uint8_t b[2];
	if (reg_read(REG_BKL, b) < 0)
		return -1;
	return b[1];
}

void kbd_backlight(int v)
{
	reg_write(REG_BKL, (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v));
}

void kbd_power_off(int delay_s)
{
	reg_write(REG_OFF, (uint8_t)delay_s);
}

/* the same, also giving the raw bytes and -1 on an I2C error (tests) */
int kbd_read_raw(int *code, int *state, uint8_t *b0, uint8_t *b1)
{
	uint8_t b[2];
	if (reg_read(REG_FIF, b) < 0)
		return -1;
	*b0 = b[0];
	*b1 = b[1];
	if (b[0] == 0 && b[1] == 0)
		return 0;
	*state = b[0];
	*code = b[1];
	return 1;
}
