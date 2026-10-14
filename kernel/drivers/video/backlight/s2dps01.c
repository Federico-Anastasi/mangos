// SPDX-License-Identifier: GPL-2.0-only
/*
 * Samsung S2DPS01 LCD backlight driver IC.
 *
 * The bootloader leaves the chip in PWM mode, where the level comes from the
 * panel over a DSI command we cannot send. In I2C mode the level is the
 * BRIGHTNESS register: 0 is dark, 0xff full scale. The supply rails the chip
 * also generates for the panel are not touched.
 */

#include <linux/backlight.h>
#include <linux/i2c.h>
#include <linux/module.h>

#define S2DPS01_BRIGHTNESS		0x23
#define S2DPS01_MODE			0x24
#define  S2DPS01_MODE_I2C		0x01

/* Samsung's normal maximum is 192/255 of full scale; above that is "high brightness mode". */
#define S2DPS01_MAX_BRIGHTNESS		192
#define S2DPS01_DEFAULT_BRIGHTNESS	96

static int s2dps01_update_status(struct backlight_device *bl)
{
	struct i2c_client *client = bl_get_data(bl);

	return i2c_smbus_write_byte_data(client, S2DPS01_BRIGHTNESS,
					 backlight_get_brightness(bl));
}

static const struct backlight_ops s2dps01_ops = {
	.options = BL_CORE_SUSPENDRESUME,
	.update_status = s2dps01_update_status,
};

static int s2dps01_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.max_brightness = S2DPS01_MAX_BRIGHTNESS,
		.brightness = S2DPS01_DEFAULT_BRIGHTNESS,
	};
	struct backlight_device *bl;
	int ret;

	/* Set the level before switching mode so the screen never goes dark. */
	ret = i2c_smbus_write_byte_data(client, S2DPS01_BRIGHTNESS, props.brightness);
	if (!ret)
		ret = i2c_smbus_write_byte_data(client, S2DPS01_MODE, S2DPS01_MODE_I2C);
	if (ret)
		return dev_err_probe(dev, ret, "backlight IC not responding\n");

	bl = devm_backlight_device_register(dev, "s2dps01", dev, client, &s2dps01_ops, &props);

	return PTR_ERR_OR_ZERO(bl);
}

static const struct of_device_id s2dps01_of_match[] = {
	{ .compatible = "samsung,s2dps01" },
	{ }
};
MODULE_DEVICE_TABLE(of, s2dps01_of_match);

static struct i2c_driver s2dps01_driver = {
	.driver = {
		.name = "s2dps01",
		.of_match_table = s2dps01_of_match,
	},
	.probe = s2dps01_probe,
};
module_i2c_driver(s2dps01_driver);

MODULE_DESCRIPTION("Samsung S2DPS01 backlight driver");
MODULE_LICENSE("GPL");
