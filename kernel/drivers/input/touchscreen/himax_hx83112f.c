// SPDX-License-Identifier: GPL-2.0-only
/*
 * Driver for the Himax HX83112F in-cell (TDDI) touchscreen controller on SPI
 *
 * The controller has no flash memory: the host has to download the firmware
 * into the controller's RAM after every reset ("zero flash").
 *
 * Register addresses, commands and sequences are taken from the "Himax
 * Android Driver Sample Code" shipped in the Samsung SM-M135F kernel sources:
 *
 * Copyright (C) 2019 Himax Corporation.
 */

#include <linux/bits.h>
#include <linux/cleanup.h>
#include <linux/crc32.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/firmware.h>
#include <linux/gpio/consumer.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#include <linux/interrupt.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pm.h>
#include <linux/spi/spi.h>
#include <linux/string.h>
#include <linux/unaligned.h>

/*
 * Must match EXTRA_FIRMWARE in the kernel configuration: the firmware is built
 * into the kernel because the controller starts before the root file system
 * is mounted.
 */
#define HX_FW_NAME			"himax/hx83112f_boe_m13_f13.bin"

/* SPI framing: every message starts with a direction byte and a bus command */
#define HX_SPI_WRITE			0xf2
#define HX_SPI_READ			0xf3

/* Bus commands */
#define HX_BUS_AHB_ADDR			0x00
#define HX_BUS_AHB_RDATA		0x08
#define HX_BUS_AHB_DIRECTION		0x0c
#define HX_BUS_AHB_DIRECTION_READ	0x00
#define HX_BUS_INCR4			0x0d
#define HX_BUS_INCR4_EN			0x10
#define HX_BUS_INCR4_AUTO		BIT(0)
#define HX_BUS_AHB_EN			0x11
#define HX_BUS_CONTI			0x13
#define HX_BUS_CONTI_EN			0x31
#define HX_BUS_EVENT_STACK		0x30
#define HX_BUS_PASSWORD_LO		0x31
#define HX_BUS_PASSWORD_LO_SAFE		0x27
#define HX_BUS_PASSWORD_HI		0x32
#define HX_BUS_PASSWORD_HI_SAFE		0x95

/* Registers behind the AHB bridge; all values are 32-bit little endian */
#define HX_REG_SET_FRAME		0x10007294
#define HX_REG_FW_BOOT_FLAG		0x100072c0
#define HX_FW_BOOT_DONE			0x000072c0
#define HX_REG_FLASH_RELOAD		0x10007f00
#define HX_FLASH_RELOAD_DISABLE		0x00009aa9
#define HX_REG_TCON_RESET		0x80020020
#define HX_REG_ADC_RESET		0x80020094
#define HX_REG_CRC_STATUS		0x80050000
#define HX_CRC_STATUS_BUSY		BIT(0)
#define HX_REG_CRC_RESULT		0x80050018
#define HX_REG_CRC_ADDR			0x80050020
#define HX_REG_CRC_CMD			0x80050028
#define HX_CRC_CMD_START		0x00990000
#define HX_CRC_CMD_WORDS		GENMASK(15, 0)
#define HX_REG_ACTIVE_RELOAD		0x90000048
#define HX_ACTIVE_RELOAD_REQUEST	0x000000ec
#define HX_ACTIVE_RELOAD_DONE		0x000001ec
#define HX_REG_FW_CTRL			0x9000005c
#define HX_FW_CTRL_RUN			0x00
#define HX_FW_CTRL_STOP			0xa5
#define HX_REG_CENTRAL_STATE		0x900000a8
#define HX_CENTRAL_STATE_SAFE_MODE	0x0c
#define HX_REG_ICID			0x900000d0
#define HX_ICID_HX83112F		0x83112f

/* Firmware image layout */
#define HX_FW_DESC_SIZE			0x400
#define HX_FW_DESC_ROW_SIZE		16
#define HX_FW_DESC_MAGIC		0x87
#define HX_FW_DESC_CFG_TABLE		0x00000a00
#define HX_FW_PART_SIZE			16

/* Touch event layout */
#define HX_MAX_POINTS			10
#define HX_EVENT_SIZE			56
#define HX_EVENT_STACK_SIZE		128
#define HX_EVENT_WIDTHS			(HX_MAX_POINTS * 4)
#define HX_EVENT_INFO			52
#define HX_EVENT_NO_POINTS		0xff
#define HX_MAX_WIDTH			200

#define HX_BLOB_HEADER_SIZE		6
#define HX_DOWNLOAD_TRIES		3
#define HX_INTERFACE_TRIES		10
#define HX_RELOAD_TRIES			5
#define HX_ZERO_EVENTS_MAX		5

/* A block of data to be written to the controller RAM in one SPI message */
struct hx_blob {
	u8 *xfer;	/* SPI frame: header, RAM address, payload */
	size_t len;	/* payload length */
	u32 addr;
	u32 crc;	/* CRC the controller must compute over the payload */
};

struct hx_ts {
	struct spi_device *spi;
	struct input_dev *input;
	struct gpio_desc *reset_gpio;
	struct touchscreen_properties props;
	/* Serializes access to the chip between the IRQ thread and resume */
	struct mutex lock;
	struct hx_blob fw;
	struct hx_blob cfg;
	unsigned int zero_events;

	u8 tx_buf[16] ____cacheline_aligned;
	u8 rx_buf[HX_EVENT_STACK_SIZE] ____cacheline_aligned;
};

static int hx_bus_write(struct hx_ts *ts, u8 cmd, const void *data, size_t len)
{
	ts->tx_buf[0] = HX_SPI_WRITE;
	ts->tx_buf[1] = cmd;
	memcpy(&ts->tx_buf[2], data, len);

	return spi_write(ts->spi, ts->tx_buf, len + 2);
}

static int hx_bus_write_byte(struct hx_ts *ts, u8 cmd, u8 val)
{
	return hx_bus_write(ts, cmd, &val, sizeof(val));
}

/* The data read is left in ts->rx_buf */
static int hx_bus_read(struct hx_ts *ts, u8 cmd, size_t len)
{
	struct spi_transfer xfers[] = {
		{ .tx_buf = ts->tx_buf, .len = 3 },
		{ .rx_buf = ts->rx_buf, .len = len },
	};

	ts->tx_buf[0] = HX_SPI_READ;
	ts->tx_buf[1] = cmd;
	ts->tx_buf[2] = 0x00;

	return spi_sync_transfer(ts->spi, xfers, ARRAY_SIZE(xfers));
}

static int hx_burst_enable(struct hx_ts *ts, bool auto_increment)
{
	int error;

	error = hx_bus_write_byte(ts, HX_BUS_CONTI, HX_BUS_CONTI_EN);
	if (error)
		return error;

	return hx_bus_write_byte(ts, HX_BUS_INCR4, HX_BUS_INCR4_EN |
				 (auto_increment ? HX_BUS_INCR4_AUTO : 0));
}

static int hx_reg_read(struct hx_ts *ts, u32 addr, u32 *val)
{
	__le32 buf = cpu_to_le32(addr);
	int error;

	error = hx_burst_enable(ts, false);
	if (error)
		return error;

	error = hx_bus_write(ts, HX_BUS_AHB_ADDR, &buf, sizeof(buf));
	if (error)
		return error;

	error = hx_bus_write_byte(ts, HX_BUS_AHB_DIRECTION,
				  HX_BUS_AHB_DIRECTION_READ);
	if (error)
		return error;

	error = hx_bus_read(ts, HX_BUS_AHB_RDATA, sizeof(buf));
	if (error)
		return error;

	*val = get_unaligned_le32(ts->rx_buf);

	return 0;
}

static int hx_reg_write(struct hx_ts *ts, u32 addr, u32 val)
{
	__le32 buf[] = { cpu_to_le32(addr), cpu_to_le32(val) };
	int error;

	error = hx_burst_enable(ts, false);
	if (error)
		return error;

	return hx_bus_write(ts, HX_BUS_AHB_ADDR, buf, sizeof(buf));
}

static void hx_pin_reset(struct hx_ts *ts)
{
	gpiod_set_value_cansleep(ts->reset_gpio, 1);
	usleep_range(5000, 5100);
	gpiod_set_value_cansleep(ts->reset_gpio, 0);
}

/* Stop the firmware and put the controller into safe mode */
static int hx_sense_off(struct hx_ts *ts)
{
	unsigned int tries;
	u32 val;
	int error;

	error = hx_reg_write(ts, HX_REG_FW_CTRL, HX_FW_CTRL_STOP);
	if (error)
		return error;

	usleep_range(10000, 11000);

	for (tries = 0; tries < 6; tries++) {
		error = hx_bus_write_byte(ts, HX_BUS_PASSWORD_LO,
					  HX_BUS_PASSWORD_LO_SAFE);
		if (error)
			return error;

		error = hx_bus_write_byte(ts, HX_BUS_PASSWORD_HI,
					  HX_BUS_PASSWORD_HI_SAFE);
		if (error)
			return error;

		error = hx_reg_read(ts, HX_REG_CENTRAL_STATE, &val);
		if (error)
			return error;

		if ((val & 0xff) == HX_CENTRAL_STATE_SAFE_MODE)
			break;

		hx_pin_reset(ts);
	}

	if ((val & 0xff) != HX_CENTRAL_STATE_SAFE_MODE) {
		dev_err(&ts->spi->dev,
			"failed to enter safe mode, central state %#010x\n", val);
		return -EIO;
	}

	error = hx_reg_write(ts, HX_REG_TCON_RESET, 0);
	if (error)
		return error;

	usleep_range(1000, 1100);

	error = hx_reg_write(ts, HX_REG_ADC_RESET, 0);
	if (error)
		return error;

	usleep_range(1000, 1100);

	return hx_reg_write(ts, HX_REG_ADC_RESET, 1);
}

static int hx_interface_on(struct hx_ts *ts)
{
	unsigned int tries;
	u8 conti = 0;
	int error;

	/* Dummy read to wake up the bus interface */
	error = hx_bus_read(ts, HX_BUS_AHB_RDATA, sizeof(u32));
	if (error)
		return error;

	for (tries = 0; tries < HX_INTERFACE_TRIES; tries++) {
		error = hx_burst_enable(ts, false);
		if (error)
			return error;

		error = hx_bus_read(ts, HX_BUS_CONTI, sizeof(u8));
		if (error)
			return error;

		conti = ts->rx_buf[0];

		error = hx_bus_read(ts, HX_BUS_INCR4, sizeof(u8));
		if (error)
			return error;

		if (conti == HX_BUS_CONTI_EN && ts->rx_buf[0] == HX_BUS_INCR4_EN)
			break;

		usleep_range(1000, 1100);
	}

	if (tries == HX_INTERFACE_TRIES)
		dev_warn(&ts->spi->dev, "bus interface not confirmed (%#04x, %#04x)\n",
			 conti, ts->rx_buf[0]);

	return 0;
}

/* Reset the controller so that it boots the firmware found in its RAM */
static int hx_sense_on(struct hx_ts *ts)
{
	unsigned int tries;
	u32 val;
	int error;

	error = hx_interface_on(ts);
	if (error)
		return error;

	error = hx_reg_write(ts, HX_REG_FW_CTRL, HX_FW_CTRL_RUN);
	if (error)
		return error;

	usleep_range(10000, 11000);
	hx_pin_reset(ts);

	for (tries = 0; tries < HX_RELOAD_TRIES; tries++) {
		error = hx_reg_write(ts, HX_REG_ACTIVE_RELOAD,
				     HX_ACTIVE_RELOAD_REQUEST);
		if (error)
			return error;

		usleep_range(1000, 1100);

		error = hx_reg_read(ts, HX_REG_ACTIVE_RELOAD, &val);
		if (error)
			return error;

		if ((val & 0xffff) == HX_ACTIVE_RELOAD_DONE)
			break;
	}

	if ((val & 0xffff) != HX_ACTIVE_RELOAD_DONE)
		dev_warn(&ts->spi->dev, "active reload not confirmed (%#010x)\n", val);

	return 0;
}

static int hx_hw_crc(struct hx_ts *ts, const struct hx_blob *blob, u32 *crc)
{
	unsigned int tries;
	u32 val;
	int error;

	error = hx_reg_write(ts, HX_REG_CRC_ADDR, blob->addr);
	if (error)
		return error;

	error = hx_reg_write(ts, HX_REG_CRC_CMD, HX_CRC_CMD_START |
			     (blob->len / sizeof(u32)));
	if (error)
		return error;

	for (tries = 0; tries <= 100; tries++) {
		error = hx_reg_read(ts, HX_REG_CRC_STATUS, &val);
		if (error)
			return error;

		if (!(val & HX_CRC_STATUS_BUSY))
			return hx_reg_read(ts, HX_REG_CRC_RESULT, crc);

		usleep_range(1000, 1100);
	}

	return -ETIMEDOUT;
}

/*
 * As in the vendor driver, a block the controller fails to verify is only
 * reported: whether the firmware runs shows once it has been started.
 */
static int hx_write_blob(struct hx_ts *ts, const struct hx_blob *blob,
			 const char *name)
{
	unsigned int tries;
	u32 crc = 0;
	int error;

	for (tries = 0; tries < HX_DOWNLOAD_TRIES; tries++) {
		error = hx_burst_enable(ts, true);
		if (error)
			return error;

		error = spi_write(ts->spi, blob->xfer,
				  HX_BLOB_HEADER_SIZE + blob->len);
		if (error)
			return error;

		error = hx_hw_crc(ts, blob, &crc);
		if (error && error != -ETIMEDOUT)
			return error;

		if (!error && crc == blob->crc)
			return 0;
	}

	dev_warn(&ts->spi->dev,
		 "%s not verified by the controller (%d): CRC %#010x, expected %#010x\n",
		 name, error, crc, blob->crc);

	return 0;
}

/* Download the firmware into the controller RAM and start it */
static int hx_start(struct hx_ts *ts)
{
	unsigned int tries;
	u32 flag;
	int error;

	hx_pin_reset(ts);

	error = hx_sense_off(ts);
	if (error)
		return error;

	error = hx_write_blob(ts, &ts->fw, "firmware");
	if (error)
		return error;

	error = hx_write_blob(ts, &ts->cfg, "configuration");
	if (error)
		return error;

	error = hx_reg_write(ts, HX_REG_SET_FRAME, 1);
	if (error)
		return error;

	/* Tell the boot code to run from RAM instead of loading from flash */
	error = hx_reg_write(ts, HX_REG_FLASH_RELOAD, HX_FLASH_RELOAD_DISABLE);
	if (error)
		return error;

	error = hx_reg_write(ts, HX_REG_FW_BOOT_FLAG, 0);
	if (error)
		return error;

	error = hx_sense_on(ts);
	if (error)
		return error;

	for (tries = 0; tries <= 200; tries++) {
		error = hx_reg_read(ts, HX_REG_FW_BOOT_FLAG, &flag);
		if (error)
			return error;

		if (flag == HX_FW_BOOT_DONE)
			return 0;

		usleep_range(10000, 11000);
	}

	/* The vendor driver carries on regardless; so do we */
	dev_warn(&ts->spi->dev, "firmware did not report start-up (%#010x)\n",
		 flag);

	return 0;
}

static int hx_detect(struct hx_ts *ts)
{
	struct device *dev = &ts->spi->dev;
	unsigned int tries;
	u32 icid;
	int error;

	/* The vendor driver holds reset for 2 ms and waits 5 ms afterwards */
	usleep_range(2000, 2100);
	gpiod_set_value_cansleep(ts->reset_gpio, 0);
	usleep_range(5000, 5100);

	error = hx_sense_off(ts);
	if (error)
		return error;

	for (tries = 0; tries < 5; tries++) {
		error = hx_reg_read(ts, HX_REG_ICID, &icid);
		if (error)
			return error;

		if (icid >> 8 == HX_ICID_HX83112F)
			break;
	}

	dev_info(dev, "IC ID %#010x\n", icid);

	if (icid >> 8 != HX_ICID_HX83112F)
		return dev_err_probe(dev, -ENODEV, "not an HX83112F\n");

	return 0;
}

static u8 *hx_blob_alloc(struct hx_ts *ts, struct hx_blob *blob, u32 addr,
			 size_t len)
{
	blob->xfer = devm_kzalloc(&ts->spi->dev, HX_BLOB_HEADER_SIZE + len,
				  GFP_KERNEL);
	if (!blob->xfer)
		return NULL;

	blob->xfer[0] = HX_SPI_WRITE;
	blob->xfer[1] = HX_BUS_AHB_ADDR;
	put_unaligned_le32(addr, &blob->xfer[2]);
	blob->addr = addr;
	blob->len = len;

	return &blob->xfer[HX_BLOB_HEADER_SIZE];
}

/* The controller computes a CRC-32C without the final inversion */
static void hx_blob_seal(struct hx_blob *blob)
{
	blob->crc = crc32c(~0, &blob->xfer[HX_BLOB_HEADER_SIZE], blob->len);
}

/* A partition table entry: RAM address, size and offset in the image */
static int hx_fw_part(const struct firmware *fw, u32 table, unsigned int index,
		      u32 *addr, u32 *len, u32 *offset)
{
	const u8 *part = fw->data + table + index * HX_FW_PART_SIZE;

	*addr = get_unaligned_le32(part);
	*len = get_unaligned_le32(part + 4);
	*offset = get_unaligned_le32(part + 8);

	if (*offset > fw->size || *len > fw->size - *offset)
		return -EINVAL;

	return 0;
}

/*
 * The image starts with a description table of checksummed 16-byte rows which
 * locates the partition table. Partition 0 is the firmware itself; the other
 * partitions are configuration data, which is merged into a single block
 * spanning from the lowest to the highest RAM address.
 */
static int hx_parse_firmware(struct hx_ts *ts, const struct firmware *fw)
{
	u32 addr, len, offset, base = U32_MAX, top = 0, top_len = 0, table = 0;
	unsigned int i, nparts;
	size_t pos, cfg_len;
	u8 *payload;
	int error;

	if (fw->size < HX_FW_DESC_SIZE || memchr_inv(fw->data, 0, 8) ||
	    fw->data[14] != HX_FW_DESC_MAGIC)
		return -EINVAL;

	for (pos = 0; pos < HX_FW_DESC_SIZE; pos += HX_FW_DESC_ROW_SIZE) {
		const u8 *row = fw->data + pos;
		u8 sum = 0;

		if (!memchr_inv(row, 0, HX_FW_DESC_ROW_SIZE))
			break;

		for (i = 0; i < HX_FW_DESC_ROW_SIZE; i++)
			sum += row[i];

		if (!sum && get_unaligned_le32(row) == HX_FW_DESC_CFG_TABLE)
			table = get_unaligned_le32(row + 4);
	}

	if (!table || table > fw->size - HX_FW_PART_SIZE)
		return -EINVAL;

	nparts = fw->data[table + 12];
	if (nparts < 2 || nparts * HX_FW_PART_SIZE > fw->size - table)
		return -EINVAL;

	error = hx_fw_part(fw, table, 0, &addr, &len, &offset);
	if (error)
		return error;

	if (!len || len % sizeof(u32) || len / sizeof(u32) > HX_CRC_CMD_WORDS)
		return -EINVAL;

	payload = hx_blob_alloc(ts, &ts->fw, addr, len);
	if (!payload)
		return -ENOMEM;

	memcpy(payload, fw->data + offset, len);
	hx_blob_seal(&ts->fw);

	/* The firmware carries its own CRC, making the total come out as 0 */
	if (ts->fw.crc)
		return -EINVAL;

	for (i = 1; i < nparts; i++) {
		error = hx_fw_part(fw, table, i, &addr, &len, &offset);
		if (error)
			return error;

		base = min(base, addr);
		if (addr > top) {
			top = addr;
			top_len = len;
		}
	}

	if (base % sizeof(u32))
		return -EINVAL;

	cfg_len = round_up(top - base + top_len, 16);
	if (cfg_len / sizeof(u32) > HX_CRC_CMD_WORDS)
		return -EINVAL;

	payload = hx_blob_alloc(ts, &ts->cfg, base, cfg_len);
	if (!payload)
		return -ENOMEM;

	for (i = 1; i < nparts; i++) {
		hx_fw_part(fw, table, i, &addr, &len, &offset);

		addr = round_down(addr, sizeof(u32)) - base;
		if (addr > cfg_len || len > cfg_len - addr)
			return -EINVAL;

		memcpy(payload + addr, fw->data + offset, len);
	}

	hx_blob_seal(&ts->cfg);

	return 0;
}

static int hx_load_firmware(struct hx_ts *ts)
{
	struct device *dev = &ts->spi->dev;
	const struct firmware *fw;
	int error;

	error = request_firmware(&fw, HX_FW_NAME, dev);
	if (error)
		return dev_err_probe(dev, error, "failed to load %s\n",
				     HX_FW_NAME);

	error = hx_parse_firmware(ts, fw);
	release_firmware(fw);
	if (error)
		return dev_err_probe(dev, error, "invalid firmware %s\n",
				     HX_FW_NAME);

	return 0;
}

static int hx_read_event(struct hx_ts *ts, size_t len)
{
	int error;

	error = hx_bus_write_byte(ts, HX_BUS_AHB_EN, 0);
	if (error)
		return error;

	error = hx_bus_read(ts, HX_BUS_EVENT_STACK, len);
	if (error)
		return error;

	return hx_bus_write_byte(ts, HX_BUS_AHB_EN, 1);
}

static void hx_release_all(struct hx_ts *ts)
{
	input_mt_sync_frame(ts->input);
	input_sync(ts->input);
}

/*
 * A controller upset by ESD answers with a constant pattern. All zeroes also
 * happens in normal operation, so only a run of such events counts.
 */
static bool hx_event_is_esd(struct hx_ts *ts, const u8 *event)
{
	if (memchr_inv(event, event[0], HX_EVENT_SIZE)) {
		ts->zero_events = 0;
		return false;
	}

	switch (event[0]) {
	case 0x00:
		if (ts->zero_events++ <= HX_ZERO_EVENTS_MAX)
			return false;
		fallthrough;
	case 0xeb:
	case 0xec:
	case 0xed:
		ts->zero_events = 0;
		return true;
	default:
		return false;
	}
}

static void hx_report_event(struct hx_ts *ts, const u8 *event)
{
	u8 info = event[HX_EVENT_INFO];
	unsigned int i;

	if (info != HX_EVENT_NO_POINTS && (info & 0x0f)) {
		for (i = 0; i < HX_MAX_POINTS; i++) {
			u16 x = get_unaligned_be16(&event[i * 4]);
			u16 y = get_unaligned_be16(&event[i * 4 + 2]);
			u8 width = event[HX_EVENT_WIDTHS + i];

			if (x > ts->props.max_x || y > ts->props.max_y)
				continue;

			input_mt_slot(ts->input, i);
			input_mt_report_slot_state(ts->input, MT_TOOL_FINGER,
						   true);
			touchscreen_report_pos(ts->input, &ts->props, x, y,
					       true);
			input_report_abs(ts->input, ABS_MT_TOUCH_MAJOR, width);
			input_report_abs(ts->input, ABS_MT_PRESSURE, width);
		}
	}

	input_mt_sync_frame(ts->input);
	input_sync(ts->input);
}

static irqreturn_t hx_irq_handler(int irq, void *dev_id)
{
	struct hx_ts *ts = dev_id;
	struct device *dev = &ts->spi->dev;
	const u8 *event = ts->rx_buf;
	unsigned int i;
	u8 sum = 0;
	u8 sel;
	int error;

	guard(mutex)(&ts->lock);

	error = hx_read_event(ts, HX_EVENT_SIZE);
	if (error) {
		dev_err_ratelimited(dev, "failed to read event: %d\n", error);
		return IRQ_HANDLED;
	}

	for (i = 0; i < HX_EVENT_SIZE; i++)
		sum += event[i];

	if (sum || !memchr_inv(event, 0, HX_EVENT_SIZE)) {
		if (!hx_event_is_esd(ts, event))
			return IRQ_HANDLED;

		dev_warn(dev, "ESD event (%#04x), reloading firmware\n",
			 event[0]);

		error = hx_start(ts);
		if (error)
			dev_err(dev, "failed to reload firmware: %d\n", error);

		hx_release_all(ts);

		return IRQ_HANDLED;
	}

	ts->zero_events = 0;

	/* Not a touch report: drop it along with the rest of the stack */
	sel = event[HX_EVENT_INFO] >> 4;
	if (sel != 0x0f && sel != 0x00) {
		hx_read_event(ts, HX_EVENT_STACK_SIZE - HX_EVENT_SIZE);
		return IRQ_HANDLED;
	}

	hx_report_event(ts, event);

	return IRQ_HANDLED;
}

static int hx_input_register(struct hx_ts *ts)
{
	struct device *dev = &ts->spi->dev;
	struct input_dev *input;
	int error;

	input = devm_input_allocate_device(dev);
	if (!input)
		return -ENOMEM;

	input->name = "Himax HX83112F Touchscreen";
	input->id.bustype = BUS_SPI;
	ts->input = input;

	input_set_capability(input, EV_ABS, ABS_MT_POSITION_X);
	input_set_capability(input, EV_ABS, ABS_MT_POSITION_Y);
	input_set_abs_params(input, ABS_MT_TOUCH_MAJOR, 0, HX_MAX_WIDTH, 0, 0);
	input_set_abs_params(input, ABS_MT_PRESSURE, 0, HX_MAX_WIDTH, 0, 0);

	touchscreen_parse_properties(input, true, &ts->props);
	if (!ts->props.max_x || !ts->props.max_y)
		return dev_err_probe(dev, -EINVAL,
				     "touchscreen-size-x/y not specified\n");

	error = input_mt_init_slots(input, HX_MAX_POINTS,
				    INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
	if (error)
		return error;

	return input_register_device(input);
}

static int hx_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct hx_ts *ts;
	int error;

	spi->mode &= ~SPI_MODE_X_MASK;
	spi->mode |= SPI_MODE_3;
	spi->bits_per_word = 8;
	error = spi_setup(spi);
	if (error)
		return dev_err_probe(dev, error, "failed to set up SPI\n");

	ts = devm_kzalloc(dev, sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;

	ts->spi = spi;
	spi_set_drvdata(spi, ts);

	error = devm_mutex_init(dev, &ts->lock);
	if (error)
		return error;

	ts->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ts->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ts->reset_gpio),
				     "failed to get reset GPIO\n");

	error = hx_detect(ts);
	if (error)
		return error;

	error = hx_load_firmware(ts);
	if (error)
		return error;

	error = hx_input_register(ts);
	if (error)
		return error;

	/*
	 * The interrupt is edge triggered: have it armed before the firmware
	 * starts so that its first event is not missed.
	 */
	error = devm_request_threaded_irq(dev, spi->irq, NULL, hx_irq_handler,
					  IRQF_ONESHOT | IRQF_NO_AUTOEN,
					  dev_name(dev), ts);
	if (error)
		return dev_err_probe(dev, error, "failed to request IRQ\n");

	error = hx_start(ts);
	if (error)
		return dev_err_probe(dev, error, "failed to start firmware\n");

	enable_irq(spi->irq);

	return 0;
}

static int hx_suspend(struct device *dev)
{
	struct hx_ts *ts = dev_get_drvdata(dev);

	disable_irq(ts->spi->irq);

	guard(mutex)(&ts->lock);

	gpiod_set_value_cansleep(ts->reset_gpio, 1);
	hx_release_all(ts);

	return 0;
}

static int hx_resume(struct device *dev)
{
	struct hx_ts *ts = dev_get_drvdata(dev);
	int error;

	scoped_guard(mutex, &ts->lock) {
		gpiod_set_value_cansleep(ts->reset_gpio, 0);
		usleep_range(5000, 5100);

		error = hx_start(ts);
		if (error)
			dev_err(dev, "failed to reload firmware: %d\n", error);
	}

	enable_irq(ts->spi->irq);

	return error;
}

static DEFINE_SIMPLE_DEV_PM_OPS(hx_pm_ops, hx_suspend, hx_resume);

static const struct spi_device_id hx_spi_id[] = {
	{ "hx83112f" },
	{ }
};
MODULE_DEVICE_TABLE(spi, hx_spi_id);

static const struct of_device_id hx_of_match[] = {
	{ .compatible = "himax,hx83112f" },
	{ }
};
MODULE_DEVICE_TABLE(of, hx_of_match);

static struct spi_driver hx_driver = {
	.driver = {
		.name = "himax_hx83112f",
		.of_match_table = hx_of_match,
		.pm = pm_sleep_ptr(&hx_pm_ops),
	},
	.probe = hx_probe,
	.id_table = hx_spi_id,
};
module_spi_driver(hx_driver);

MODULE_DESCRIPTION("Himax HX83112F zero-flash SPI touchscreen driver");
MODULE_FIRMWARE(HX_FW_NAME);
MODULE_LICENSE("GPL");
