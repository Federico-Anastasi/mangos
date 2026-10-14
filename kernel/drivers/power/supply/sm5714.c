// SPDX-License-Identifier: GPL-2.0-only
/*
 * Silicon Mitus SM5714 switching charger and fuel gauge.
 *
 * Register layout and conversion formulas come from Samsung's downstream
 * sm5714_charger / sm5714_fuelgauge drivers (Exynos 850, kernel 4.19).
 * The charger answers at the address given in the device tree, the fuel gauge
 * is a second I2C function of the same chip (SM5714_FG_ADDR).
 */

#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/power_supply.h>
#include <linux/workqueue.h>

#define SM5714_FG_ADDR			0x71

/* Charger registers (8 bit) */
#define SM5714_CHG_STATUS1		0x0d
#define  SM5714_STATUS1_VBUSPOK		BIT(0)
#define SM5714_CHG_STATUS2		0x0e
#define  SM5714_STATUS2_NOBAT		BIT(2)
#define  SM5714_STATUS2_CHGON		BIT(3)
#define  SM5714_STATUS2_TOPOFF		BIT(5)
#define SM5714_CHG_CNTL1		0x13
#define  SM5714_CNTL1_ENQ4FET		BIT(3)
#define SM5714_CHG_VBUSCNTL		0x15
#define  SM5714_VBUSCNTL_LIMIT		GENMASK(6, 0)
#define SM5714_CHG_CHGCNTL2		0x18
#define SM5714_CHG_CHGCNTL4		0x1a
#define  SM5714_CHGCNTL4_BATREG		GENMASK(5, 0)
#define SM5714_CHG_DEVICEID		0x50

/* Fuel gauge registers (16 bit) and SRAM addresses */
#define SM5714_FG_SRAM_RADDR		0x8c
#define SM5714_FG_SRAM_RDATA		0x8d
#define SM5714_FG_SRAM_SOC		0x00
#define SM5714_FG_SRAM_VBAT		0x03
#define SM5714_FG_SRAM_CURRENT		0x05
#define SM5714_FG_SRAM_TEMPERATURE	0x07
#define SM5714_FG_SRAM_CURRENT_AVG	0x09

#define SM5714_POLL_MS			30000
#define SM5714_RECHARGE_HYST		5

struct sm5714 {
	struct i2c_client *chg;
	struct i2c_client *fg;
	struct mutex fg_lock;
	struct mutex chg_lock;
	struct power_supply *battery;
	struct power_supply *usb;
	struct delayed_work poll;
	int end_threshold;
	int last_capacity;
	int last_status;
};

static int sm5714_chg_update(struct sm5714 *sm, u8 reg, u8 mask, u8 val)
{
	int ret;

	mutex_lock(&sm->chg_lock);
	ret = i2c_smbus_read_byte_data(sm->chg, reg);
	if (ret >= 0)
		ret = i2c_smbus_write_byte_data(sm->chg, reg, (ret & ~mask) | (val & mask));
	mutex_unlock(&sm->chg_lock);

	return ret;
}

static int sm5714_fg_read_sram(struct sm5714 *sm, u8 addr)
{
	int ret;

	mutex_lock(&sm->fg_lock);
	ret = i2c_smbus_write_word_data(sm->fg, SM5714_FG_SRAM_RADDR, addr);
	if (!ret)
		ret = i2c_smbus_read_word_data(sm->fg, SM5714_FG_SRAM_RDATA);
	mutex_unlock(&sm->fg_lock);

	return ret;
}

/* Sign-magnitude raw value to microamps: 2044 LSB per amp. */
static int sm5714_raw_to_ua(int raw)
{
	int ua = (raw & 0x7fff) * 1000000LL / 2044;

	return (raw & 0x8000) ? -ua : ua;
}

static int sm5714_get_capacity(struct sm5714 *sm, int *capacity)
{
	int raw = sm5714_fg_read_sram(sm, SM5714_FG_SRAM_SOC);

	if (raw < 0)
		return raw;
	*capacity = clamp(raw >> 8, 0, 100);

	return 0;
}

static int sm5714_get_status(struct sm5714 *sm, int *status)
{
	int st1, st2;

	st1 = i2c_smbus_read_byte_data(sm->chg, SM5714_CHG_STATUS1);
	if (st1 < 0)
		return st1;
	st2 = i2c_smbus_read_byte_data(sm->chg, SM5714_CHG_STATUS2);
	if (st2 < 0)
		return st2;

	if (!(st1 & SM5714_STATUS1_VBUSPOK))
		*status = POWER_SUPPLY_STATUS_DISCHARGING;
	else if (st2 & SM5714_STATUS2_TOPOFF)
		*status = POWER_SUPPLY_STATUS_FULL;
	else if (st2 & SM5714_STATUS2_CHGON)
		*status = POWER_SUPPLY_STATUS_CHARGING;
	else
		*status = POWER_SUPPLY_STATUS_NOT_CHARGING;

	return 0;
}

static int sm5714_battery_get_property(struct power_supply *psy,
				       enum power_supply_property psp,
				       union power_supply_propval *val)
{
	struct sm5714 *sm = power_supply_get_drvdata(psy);
	int ret, raw;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		return sm5714_get_status(sm, &val->intval);
	case POWER_SUPPLY_PROP_PRESENT:
		ret = i2c_smbus_read_byte_data(sm->chg, SM5714_CHG_STATUS2);
		if (ret < 0)
			return ret;
		val->intval = !(ret & SM5714_STATUS2_NOBAT);
		return 0;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		return 0;
	case POWER_SUPPLY_PROP_CAPACITY:
		return sm5714_get_capacity(sm, &val->intval);
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		raw = sm5714_fg_read_sram(sm, SM5714_FG_SRAM_VBAT);
		if (raw < 0)
			return raw;
		/* 2.7 V offset, 10.9 LSB per millivolt, sign-magnitude */
		if (raw & 0x8000)
			val->intval = 2700000 - (raw & 0x7fff) * 10000 / 109;
		else
			val->intval = 2700000 + raw * 10000 / 109;
		return 0;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
	case POWER_SUPPLY_PROP_CURRENT_AVG:
		raw = sm5714_fg_read_sram(sm, psp == POWER_SUPPLY_PROP_CURRENT_NOW ?
					  SM5714_FG_SRAM_CURRENT : SM5714_FG_SRAM_CURRENT_AVG);
		if (raw < 0)
			return raw;
		val->intval = sm5714_raw_to_ua(raw);
		return 0;
	case POWER_SUPPLY_PROP_TEMP:
		raw = sm5714_fg_read_sram(sm, SM5714_FG_SRAM_TEMPERATURE);
		if (raw < 0)
			return raw;
		/* tenths of a degree Celsius */
		val->intval = ((raw & 0x7fff) * 10 * 2989) >> 19;
		if (raw & 0x8000)
			val->intval = -val->intval;
		return 0;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		ret = i2c_smbus_read_byte_data(sm->chg, SM5714_CHG_CHGCNTL2);
		if (ret < 0)
			return ret;
		val->intval = 109375 + (clamp(ret, 0x07, 0xe0) - 7) * 15625;
		return 0;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE:
		ret = i2c_smbus_read_byte_data(sm->chg, SM5714_CHG_CHGCNTL4);
		if (ret < 0)
			return ret;
		raw = ret & SM5714_CHGCNTL4_BATREG;
		if (raw < 4)
			val->intval = 3700000 + raw * 50000;
		else if (raw < 6)
			val->intval = 3900000 + (raw - 4) * 100000;
		else
			val->intval = 4050000 + (raw - 6) * 10000;
		return 0;
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD:
		val->intval = sm->end_threshold;
		return 0;
	default:
		return -EINVAL;
	}
}

static int sm5714_battery_set_property(struct power_supply *psy,
				       enum power_supply_property psp,
				       const union power_supply_propval *val)
{
	struct sm5714 *sm = power_supply_get_drvdata(psy);
	int code;

	switch (psp) {
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		if (val->intval < 109375 || val->intval > 3500000)
			return -EINVAL;
		code = 7 + (val->intval - 109375) / 15625;
		return i2c_smbus_write_byte_data(sm->chg, SM5714_CHG_CHGCNTL2, code);
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD:
		if (val->intval < 20 || val->intval > 100)
			return -EINVAL;
		sm->end_threshold = val->intval;
		mod_delayed_work(system_wq, &sm->poll, 0);
		return 0;
	default:
		return -EINVAL;
	}
}

static int sm5714_battery_property_is_writeable(struct power_supply *psy,
						enum power_supply_property psp)
{
	return psp == POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT ||
	       psp == POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD;
}

static const enum power_supply_property sm5714_battery_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CURRENT_AVG,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE,
	POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD,
};

static const struct power_supply_desc sm5714_battery_desc = {
	.name = "sm5714-battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = sm5714_battery_props,
	.num_properties = ARRAY_SIZE(sm5714_battery_props),
	.get_property = sm5714_battery_get_property,
	.set_property = sm5714_battery_set_property,
	.property_is_writeable = sm5714_battery_property_is_writeable,
};

static int sm5714_usb_get_property(struct power_supply *psy,
				   enum power_supply_property psp,
				   union power_supply_propval *val)
{
	struct sm5714 *sm = power_supply_get_drvdata(psy);
	int ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		ret = i2c_smbus_read_byte_data(sm->chg, SM5714_CHG_STATUS1);
		if (ret < 0)
			return ret;
		val->intval = !!(ret & SM5714_STATUS1_VBUSPOK);
		return 0;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		ret = i2c_smbus_read_byte_data(sm->chg, SM5714_CHG_VBUSCNTL);
		if (ret < 0)
			return ret;
		val->intval = 100000 + (ret & SM5714_VBUSCNTL_LIMIT) * 25000;
		return 0;
	default:
		return -EINVAL;
	}
}

static int sm5714_usb_set_property(struct power_supply *psy,
				   enum power_supply_property psp,
				   const union power_supply_propval *val)
{
	struct sm5714 *sm = power_supply_get_drvdata(psy);

	if (psp != POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT)
		return -EINVAL;
	if (val->intval < 100000 || val->intval > 3275000)
		return -EINVAL;

	return sm5714_chg_update(sm, SM5714_CHG_VBUSCNTL, SM5714_VBUSCNTL_LIMIT,
				 (val->intval - 100000) / 25000);
}

static int sm5714_usb_property_is_writeable(struct power_supply *psy,
					    enum power_supply_property psp)
{
	return psp == POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT;
}

static const enum power_supply_property sm5714_usb_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
};

static const struct power_supply_desc sm5714_usb_desc = {
	.name = "sm5714-usb",
	.type = POWER_SUPPLY_TYPE_USB,
	.properties = sm5714_usb_props,
	.num_properties = ARRAY_SIZE(sm5714_usb_props),
	.get_property = sm5714_usb_get_property,
	.set_property = sm5714_usb_set_property,
	.property_is_writeable = sm5714_usb_property_is_writeable,
};

/*
 * Charging is gated by the Q4 FET: stop it at the end threshold, resume once
 * the battery has dropped SM5714_RECHARGE_HYST points below it. The buck keeps
 * feeding the system from VBUS either way.
 */
static void sm5714_poll(struct work_struct *work)
{
	struct sm5714 *sm = container_of(work, struct sm5714, poll.work);
	int capacity, status, cntl1;

	if (sm5714_get_capacity(sm, &capacity) || sm5714_get_status(sm, &status))
		goto out;

	cntl1 = i2c_smbus_read_byte_data(sm->chg, SM5714_CHG_CNTL1);
	if (cntl1 >= 0) {
		bool enabled = cntl1 & SM5714_CNTL1_ENQ4FET;

		if (enabled && sm->end_threshold < 100 && capacity >= sm->end_threshold)
			sm5714_chg_update(sm, SM5714_CHG_CNTL1, SM5714_CNTL1_ENQ4FET, 0);
		else if (!enabled && (sm->end_threshold == 100 ||
				      capacity <= sm->end_threshold - SM5714_RECHARGE_HYST))
			sm5714_chg_update(sm, SM5714_CHG_CNTL1, SM5714_CNTL1_ENQ4FET,
					  SM5714_CNTL1_ENQ4FET);
	}

	if (capacity != sm->last_capacity || status != sm->last_status) {
		sm->last_capacity = capacity;
		sm->last_status = status;
		power_supply_changed(sm->battery);
		power_supply_changed(sm->usb);
	}
out:
	schedule_delayed_work(&sm->poll, msecs_to_jiffies(SM5714_POLL_MS));
}

static void sm5714_stop_poll(void *data)
{
	struct sm5714 *sm = data;

	cancel_delayed_work_sync(&sm->poll);
}

static int sm5714_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct power_supply_config cfg = {};
	struct sm5714 *sm;
	int ret;

	sm = devm_kzalloc(dev, sizeof(*sm), GFP_KERNEL);
	if (!sm)
		return -ENOMEM;

	sm->chg = client;
	sm->end_threshold = 100;
	sm->last_capacity = -1;
	sm->last_status = -1;
	mutex_init(&sm->fg_lock);
	mutex_init(&sm->chg_lock);
	/* Before the supplies are registered: writing the end threshold queues this work. */
	INIT_DELAYED_WORK(&sm->poll, sm5714_poll);

	ret = i2c_smbus_read_byte_data(client, SM5714_CHG_DEVICEID);
	if (ret < 0)
		return dev_err_probe(dev, ret, "charger not responding\n");
	dev_info(dev, "SM5714 device id 0x%02x\n", ret);

	sm->fg = devm_i2c_new_dummy_device(dev, client->adapter, SM5714_FG_ADDR);
	if (IS_ERR(sm->fg))
		return dev_err_probe(dev, PTR_ERR(sm->fg), "cannot claim fuel gauge address\n");

	cfg.drv_data = sm;
	cfg.fwnode = dev_fwnode(dev);

	sm->battery = devm_power_supply_register(dev, &sm5714_battery_desc, &cfg);
	if (IS_ERR(sm->battery))
		return dev_err_probe(dev, PTR_ERR(sm->battery), "cannot register battery\n");

	sm->usb = devm_power_supply_register(dev, &sm5714_usb_desc, &cfg);
	if (IS_ERR(sm->usb))
		return dev_err_probe(dev, PTR_ERR(sm->usb), "cannot register usb supply\n");

	/* Registered after the supplies, so on removal the work stops before they go away. */
	ret = devm_add_action_or_reset(dev, sm5714_stop_poll, sm);
	if (ret)
		return ret;
	schedule_delayed_work(&sm->poll, 0);

	return 0;
}

static const struct of_device_id sm5714_of_match[] = {
	{ .compatible = "siliconmitus,sm5714" },
	{ }
};
MODULE_DEVICE_TABLE(of, sm5714_of_match);

static struct i2c_driver sm5714_driver = {
	.driver = {
		.name = "sm5714",
		.of_match_table = sm5714_of_match,
	},
	.probe = sm5714_probe,
};
module_i2c_driver(sm5714_driver);

MODULE_DESCRIPTION("Silicon Mitus SM5714 charger and fuel gauge");
MODULE_LICENSE("GPL");
