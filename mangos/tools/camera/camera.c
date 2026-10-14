// SPDX-License-Identifier: GPL-2.0-only
/*
 * M13 cameras: a raw shot on command. Two sensors, each with its own device:
 *   /dev/camera-rear    rear, Samsung S5KJN1, 4080x3072, receiver CSIS1
 *   /dev/camera-front   front, GalaxyCore GC08A3, 3264x2448, receiver CSIS0
 *
 * Write what to do to the device, one or more words separated by spaces:
 *   exposure=<rows>   exposure time in sensor rows
 *   gain=<N>          analog gain (rear up to 64, front up to 16)
 *   bars=<0..4>       sensor test pattern instead of what it sees (2 = bars)
 *   focus=<0..1023>   lens position: 0 far, 1023 near (rear only)
 *   shoot             take a shot with the current settings; the write returns when done
 *   start             power the sensor on and let it stream: this is the preview. While on,
 *                     exposure, gain and focus apply immediately, and what is read is the
 *                     incoming frame (it may be half-written: fine to look at, not for a
 *                     photo). "shoot" while on takes one whole frame and powers off.
 * and read the last shot from it: each pixel (10 bits) in 12 bits, two pixels in three bytes, the
 * row rounded up to 16 bytes (6128 bytes rear, 4896 front). Only one of the two devices can be
 * open at a time: the cameras share the receiver, the memory output and the block clock.
 * The frame memory is held as long as the device stays open.
 * Colors, brightness and format are up to the reader (mangos-photo): there is no image processor.
 * The state of the last shot is in /sys/kernel/debug/camera-rear and camera-front.
 *
 * Power-up: the sequences from the Samsung M135F sources (camera_m13_common_exynos850.dtsi).
 * The sensor clock (CIS_CLK0 or CIS_CLK1 of CMU_TOP, 26 MHz) is not in the kernel's clock
 * driver: it is enabled here by writing the four registers Samsung writes
 * (drivers/soc/samsung/cal-if/exynos3830/cmucal-sfr.c).
 *
 * The receivers and their memory output live in a chip block ("IS" power domain, CMU_IS clock)
 * that the kernel only partly knows, and that the bootloader leaves powered with its clocks
 * open: if it were not, reading one of its registers would hang the kernel, so the power state
 * is checked first. They are configured as Samsung does (drivers/media/platform/exynos/camera,
 * csi2 and hardware/api). The sensor tables are those of the kernel drivers (s5kjn1, gc08a3),
 * corrected for 26 MHz.
 *
 * Rear focus: a DW9808 motor moves the lens. It is on another bus (hsi2c_0, address 0x0c) with
 * its own supply (LDO7 of the S2MPB03), on only during a shot. The registers are those of the
 * Samsung driver (is-actuator-dw9808.c). Where the device tree does not enable that bus, the
 * lens stays at rest and focus= has no effect.
 */
#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_reserved_mem.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/seq_file.h>
#include <linux/sizes.h>
#include <linux/soc/samsung/exynos-pmu.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include <dt-bindings/clock/exynos850.h>

/* A write to the sensor: for the GC08A3 the value is 8 bits, for the S5KJN1 16. */
struct reg_entry {
	u16 reg, value;
};

#include "s5kjn1_tables.h"
#include "gc08a3_tables.h"

/* Sensor clocks in CMU_TOP: for each one a selector, a divider, a gate, an output. */
#define CMU_TOP			0x120e0000
#define CMU_TOP_SIZE		0x4000
#define MUX_SELECT		BIT(0)		/* 0 = 26 MHz oscillator */
#define DIV_MASK		GENMASK(4, 0)	/* divider minus one */
#define GATE_ON			BIT(21)
#define QCH_ON			BIT(0)		/* output to the pin */

/* The camera block: power in the PMU, clocks in CMU_IS. */
#define PMU_IS_STATUS		0x2104		/* bit 0: block powered */
#define CMU_IS			0x14500000
#define CMU_IS_SIZE		0x4000
#define IS_GATE_ON		BIT(21)

/*
 * The frame. The "packed" output puts every pixel in 12 bits even though it has 10, and rounds
 * the row up to 16 bytes (the V4L2 format of the same name uses 10).
 * The output does not know the height: it writes the rows that arrive. After the frame there is
 * a guard area filled with a known value: if it changes, more data arrived than expected.
 */
#define GUARD_ROWS		64
#define GUARD_FILL		0xa5
/* At the head of every row, before the shot: what stays like this was not written. */
#define EMPTY_FILL		0x5a
#define ROW_HEAD		64
/*
 * The block clock. The bootloader leaves it at 50 MHz, but the rear receiver has to handle 577
 * million pixels per second, two per cycle: at 50 MHz it overflows and nothing reaches memory.
 * During capture it runs at 400 MHz, and the register clock (a quarter) at 100.
 */
#define BLOCK_HZ		400000000
#define REGS_HZ			100000000
/* The first frame after the sensor starts arrives half-written: the third one is kept. */
#define FRAMES			3

/* Receiver. */
#define CSIS_CMN_CTRL		0x0004
#define CSIS_PHY_SEL		BIT(21)
#define CSIS_QCHANNEL_EN	BIT(20)
#define CSIS_INTERLEAVE		GENMASK(11, 10)
/*
 * Packets are separated by virtual channel, as Samsung does. With 0 (a single channel) the
 * receiver sees frame start and end but drops the rows: zero rows in memory.
 */
#define CSIS_PER_CHANNEL	2
#define CSIS_LANES		GENMASK(9, 8)
#define CSIS_SW_RESET		BIT(1)
#define CSIS_EN			BIT(0)
#define CSIS_UPD_SDW		0x000c
#define CSIS_INT_SRC0		0x0014		/* link errors */
#define CSIS_INT_SRC1		0x001c		/* frame start and end, lost rows */
#define CSIS_ISP_CONFIG_CH0	0x0040
#define CSIS_ISP_RESOL_CH0	0x0044
#define CSIS_FRM_CNT_CH0	0x0500
#define CSIS_DBG_OPTION		0x0690
#define CSIS_PHY_STATUS		0x0700
#define CSIS_PHY_CMN_CTRL	0x0704
#define CSIS_RAW10_DOUBLE	0x10ac		/* channel 0, data type 0x2b, two pixels per cycle */
/* The D-PHY of a receiver, four data lanes. */
#define DPHY_SIZE		0x3000
#define DPHY_CLOCK		0x1018
#define DPHY_LANE(i, reg)	((reg) + 0x400 * (i))
#define DPHY_ALIGN		0x143c		/* bit 6:5 */
#define DPHY_MODE		0x14ac		/* bit 2 */
#define DPHY_SETTLE		0x14b0		/* bit 7:0 */
#define DPHY_CALIBRATE		0x14e0		/* bit 0 */
/* Memory output: common block, DMA0 channel 0, DMA0 common. */
#define DMA_COMMON		0x14430000
#define DMA0			0x14431000
#define DMA_CTRL		0x00
#define DMA_FMT			0x04
#define DMA_FMT_PACKED		BIT(3)
#define DMA_ADDR1		0x10
#define DMA_ACT_CTRL		0x30
#define DMA_RESOL		0x40
#define DMA_FCNTSEQ		0x50
#define DMA0_COMMON		0x400		/* inside the same DMA0 mapping */
#define DMA_INT_SRC		0x08
#define DMA_FRAME_END		BIT(8)
#define DMA_STOPPED		BIT(16)
#define DMA_CMN_CTRL		0x14
#define DMA_ERR_CODE		0x18
#define DMA_OTF_FORMAT		0x1c
/* System registers of the block: which receiver goes to output 0, and the D-PHY resets. */
#define SYSREG_IS		0x14520000
#define GLUE_OUTPUT0		0x0428		/* bit 1:0: 0 = CSIS0, 1 = CSIS1 */
#define PHY_RESET		0x0500		/* one bit per D-PHY: 0 = in reset */
#define PMU_PHY			0x0730		/* bit 0: camera D-PHYs powered */
/* The block's sysmmu: it must be off, because the output is given a physical address. */
#define SYSMMU_IS0		0x14550000
#define SYSMMU_SIZE		0x9000
#define MMU_CTRL		0x0000
#define MMU_CTRL_VM		0x8000
#define CMU_IS_SYSMMU		0x2098

/* Registers the two sensors have in common. */
#define REG_STREAM		0x0100		/* 8 bits: 1 = the sensor sends frames */
#define REG_EXPOSURE		0x0202		/* 16 bits, in rows */
#define REG_GAIN		0x0204		/* 16 bits, analog gain in sensor units */
#define REG_TOTAL_ROWS		0x0340		/* 16 bits */
#define REG_WIDTH		0x034c		/* 16 bits */
#define REG_HEIGHT		0x034e		/* 16 bits */
/* Between the end of the exposure and the end of the frame the sensors want a few rows. */
#define ROW_MARGIN		16

/* Focus motor. */
#define FOCUS_BUS		0x138a0000
#define FOCUS_ADDRESS		0x0c
#define FOCUS_CONTROL		0x02		/* bit 0: at rest; bit 1: damped movement */
#define FOCUS_POSITION		0x03		/* 10 bits over two registers, the two high ones first */
#define FOCUS_MODE		0x06
#define FOCUS_MODE_SAMSUNG	0x61		/* "SAC3" damping, lens period for 1 */
#define FOCUS_PERIOD		0x07
#define FOCUS_PERIOD_SAMSUNG	0x36
#define FOCUS_MAX		1023
/* The lens takes a few tens of milliseconds to arrive and stop oscillating. */
#define FOCUS_SETTLE_MS	60

/*
 * What in the kernel tables depends on the clock (24 MHz), brought back to Samsung's values for
 * this phone's 26 MHz. S5KJN1 (is-cis-jn1-setA.h, mode 4080x3060): same pixel clock (563 MHz)
 * and same speed (1443 Mbit/s per lane).
 */
static const struct reg_entry s5kjn1_26mhz[] = {
	{ 0x0136, 0x1a00 },		/* input clock: 26.0 MHz */
	{ 0x0306, 0x0082 },		/* pixel multiplier */
	{ 0x0310, 0x006f },		/* link multiplier */
	{ 0x0342, 0x1450 },		/* total columns: 5200 */
};

/*
 * GC08A3 (is-cis-gc08a3-setA.h, mode 3264x2448): with these six entries the sensor state is,
 * register by register, Samsung's: pixels at 280.8 MHz, 669.5 Mbit/s per lane.
 */
static const struct reg_entry gc08a3_26mhz[] = {
	{ 0x0326, 0x36 }, { 0x0327, 0x05 },	/* pixel clock: 26 x 54 / 5 */
	{ 0x0336, 0x67 },			/* link: 26 x 103 / 4 */
	{ 0x0343, 0x2c },			/* total columns: 3672 */
	{ 0x0180, 0x67 }, { 0x009a, 0x00 },
};

/* Only one device open at a time, for the whole module: the cameras share the receiver. */
static unsigned long busy;
/* The memory output, shared by both cameras, is stopped: the buffer can be freed. */
static bool dma_idle = true;

struct camera;

/* What tells one sensor from the other. */
struct sensor_type {
	const char *name;		/* of the device and of the status file */
	const char *sensor;
	/* Power-up. */
	u32 clock[4];			/* CMU_TOP: selector, divider, gate, output */
	bool reset_before_clock;	/* reset is released before the clock is given */
	u16 supply_delay_us[3];		/* wait after I/O, core, analog supplies */
	u8 power_on_ms;			/* wait before talking to the sensor */
	u16 id_reg, id;
	/* Programming. */
	bool data8;			/* registers are 8 bits: a 16-bit one is two writes */
	const struct reg_entry *init, *mode, *fixup;
	u16 n_init, n_mode, n_fixup;
	int (*bars)(struct camera *f);
	u16 width, height, total_cols, total_rows, max_rows;
	u32 pixel_khz;
	u16 min_exposure;
	bool even_rows;			/* exposure and total rows only even */
	u16 max_gain, gain_scale;
	bool has_focus;
	/* Receiver. */
	u32 csis, phy, phy_reset, glue_mask, glue_output0;
	u8 settle;
	u32 gates[3];			/* CMU_IS: register bus, clock, automatic request */
};

/* The CMU_IS registers whose state is shown: three of the receiver, then the common ones. */
static const struct { u32 reg; bool gate; const char *name; } is_common[] = {
	{ 0x0600, false, "block clock selection (bit 4: 1 = fast)" },
	{ 0x2010, true, "register bus of the memory output" },
	{ 0x2050, true, "memory output" },
	{ 0x206c, true, "block output to memory, first stage" },
	{ 0x20a4, true, "block output to memory, second stage" },
	{ 0x20a0, true, "system registers of the block" },
};
static const char * const gate_names[3] = {
	"register bus of the receiver", "receiver", "automatic clock request of the receiver",
};
static const char * const clock_names[4] = { "selector", "divider", "gate", "output" };

struct camera {
	const struct sensor_type *type;
	struct i2c_client *bus;
	struct regulator *vddio, *vddd, *vdda, *vddaf;
	struct i2c_client *actuator;	/* NULL: no focus */
	struct gpio_desc *reset, *select;
	void __iomem *cmu, *cmu_is, *csis, *phy, *common, *dma, *sysreg, *mmu;
	struct regmap *pmu;
	struct dentry *status;
	struct miscdevice miscdev;
	struct mutex shot_lock;		/* settings do not change in the middle of a shot */
	u32 is_powered, is_gates[3], is_before[ARRAY_SIZE(is_common)];
	size_t row_bytes, frame_bytes, guard_bytes;
	/* The next shot. */
	unsigned int exposure, gain, bars, focus;
	int focus_result;
	/* The last shot. */
	unsigned int shots, shots_here, total_rows_used, exposure_used, gain_used, retried_writes;
	int shot_result;
	void *frame;			/* taken when the device is opened */
	dma_addr_t frame_phys;
	bool on;			/* the sensor is streaming (preview, or shot in progress) */
	u32 frame_ms, phy_before;
	const char *capture_phase;
	int capture_result[FRAMES];
	u32 mmu_ctrl, mmu_vm, c_phy, c_frames, c_dma_err, c_dma_state;
	u32 c_int0[FRAMES], c_int1[FRAMES], c_dma[FRAMES], c_before0, c_before1;
	u32 phy_read[5], detail[16], shadow[8], dma_active[4];
	unsigned long block_hz_before, block_hz, regs_hz;
	u16 width_read, height_read;
	size_t guard_touched, guard_touched_end, rows_written;
	u32 before[4];			/* the sensor clock registers at load time */
	int id_result;			/* of reading the identifier: 0, or the error */
	u16 id;
};

static void reg_update(void __iomem *reg, u32 mask, u32 value)
{
	writel((readl(reg) & ~mask) | value, reg);
}

static void clock_set(struct camera *f, bool on)
{
	const u32 *reg = f->type->clock;

	if (on) {
		reg_update(f->cmu + reg[0], MUX_SELECT, 0);
		reg_update(f->cmu + reg[1], DIV_MASK, 0);
		reg_update(f->cmu + reg[2], GATE_ON, GATE_ON);
		reg_update(f->cmu + reg[3], QCH_ON, QCH_ON);
	} else {
		reg_update(f->cmu + reg[3], QCH_ON, f->before[3] & QCH_ON);
		reg_update(f->cmu + reg[2], GATE_ON, f->before[2] & GATE_ON);
	}
}

/* ---- Sensor bus ---- */

/* Two bytes read from a register: a 16-bit value, high byte first. */
static int sensor_read16(struct camera *f, u16 reg, u16 *value)
{
	u8 address[2] = { reg >> 8, reg & 0xff }, data[2];
	struct i2c_msg msgs[2] = {
		{ .addr = f->bus->addr, .len = 2, .buf = address },
		{ .addr = f->bus->addr, .flags = I2C_M_RD, .len = 2, .buf = data },
	};
	int ret = i2c_transfer(f->bus->adapter, msgs, 2);

	if (ret != 2)
		return ret < 0 ? ret : -EIO;
	*value = data[0] << 8 | data[1];
	return 0;
}

/* A write to the sensor. Sometimes it does not acknowledge the first one after power-up: retry. */
static int sensor_send(struct camera *f, const u8 *data, int n)
{
	int ret = -EIO, attempt;

	for (attempt = 0; attempt < 3; attempt++) {
		ret = i2c_master_send(f->bus, data, n);
		if (ret == n)
			return 0;
		f->retried_writes++;
		usleep_range(1000, 2000);
	}
	return ret < 0 ? ret : -EIO;
}

static int sensor_write8(struct camera *f, u16 reg, u8 value)
{
	u8 data[3] = { reg >> 8, reg & 0xff, value };

	return sensor_send(f, data, sizeof(data));
}

/* A 16-bit value: a single write, or two 8-bit ones if the sensor has 8-bit registers. */
static int sensor_write16(struct camera *f, u16 reg, u16 value)
{
	u8 data[4] = { reg >> 8, reg & 0xff, value >> 8, value & 0xff };

	if (f->type->data8)
		return sensor_write8(f, reg, value >> 8) ?: sensor_write8(f, reg + 1, value & 0xff);
	return sensor_send(f, data, sizeof(data));
}

static int sensor_table(struct camera *f, const struct reg_entry *entries, size_t n)
{
	size_t i;
	int ret;

	for (i = 0; i < n; i++) {
		ret = f->type->data8 ? sensor_write8(f, entries[i].reg, entries[i].value) :
				       sensor_write16(f, entries[i].reg, entries[i].value);
		if (ret)
			return ret;
	}
	return 0;
}

/* ---- Power-up ---- */

/* The Samsung power-up sequence, up to reading the identifier. */
static int sensor_power_on(struct camera *f)
{
	const struct sensor_type *t = f->type;
	struct device *dev = &f->bus->dev;
	int ret;

	gpiod_set_value_cansleep(f->reset, 1);
	usleep_range(500, 1000);
	ret = regulator_enable(f->vddio);
	if (ret)
		return ret;
	usleep_range(t->supply_delay_us[0] + 1, t->supply_delay_us[0] + 100);
	ret = regulator_enable(f->vddd);
	if (ret)
		goto off_vddio;
	usleep_range(t->supply_delay_us[1] + 1, t->supply_delay_us[1] + 100);
	ret = regulator_enable(f->vdda);
	if (ret)
		goto off_vddd;
	usleep_range(t->supply_delay_us[2] + 500, t->supply_delay_us[2] + 1000);
	/* Where two sensors share a receiver, a pin says which of the two is connected to it. */
	if (f->select) {
		gpiod_set_value_cansleep(f->select, 1);
		usleep_range(500, 1000);
	}
	if (t->reset_before_clock) {
		gpiod_set_value_cansleep(f->reset, 0);
		usleep_range(1000, 1500);
		clock_set(f, true);
	} else {
		clock_set(f, true);
		usleep_range(1500, 2000);
		gpiod_set_value_cansleep(f->reset, 0);
	}
	msleep(t->power_on_ms);

	f->id_result = sensor_read16(f, t->id_reg, &f->id);
	if (f->id_result)
		dev_err(dev, "sensor does not answer on I2C (%d)\n", f->id_result);
	else if (f->id != t->id)
		dev_warn(dev, "identifier %#06x: NOT the expected one (%#06x)\n", f->id, t->id);
	return 0;

off_vddd:
	regulator_disable(f->vddd);
off_vddio:
	regulator_disable(f->vddio);
	return ret;
}

/* Everything off, in reverse order. */
static void sensor_power_off(struct camera *f)
{
	if (f->type->reset_before_clock) {
		clock_set(f, false);
		usleep_range(1000, 1500);
		gpiod_set_value_cansleep(f->reset, 1);
	} else {
		gpiod_set_value_cansleep(f->reset, 1);
		clock_set(f, false);
	}
	usleep_range(1000, 1500);
	regulator_disable(f->vdda);
	regulator_disable(f->vddd);
	regulator_disable(f->vddio);
	if (f->select)
		gpiod_set_value_cansleep(f->select, 0);
}

/* ---- Focus ---- */

/* Power the motor on and move the lens to the requested position. Without a motor, does nothing. */
static int focus_set(struct camera *f)
{
	const u8 startup[][2] = {
		{ FOCUS_CONTROL, 1 }, { FOCUS_CONTROL, 0 },
	}, mode[][2] = {
		{ FOCUS_CONTROL, 2 }, { FOCUS_MODE, FOCUS_MODE_SAMSUNG }, { FOCUS_PERIOD, FOCUS_PERIOD_SAMSUNG },
	};
	u8 position[3] = { FOCUS_POSITION, f->focus >> 8, f->focus & 0xff };
	int ret, i;

	if (!f->actuator)
		return 0;
	ret = regulator_enable(f->vddaf);
	if (ret)
		return ret;
	usleep_range(5000, 6000);
	for (i = 0; !ret && i < ARRAY_SIZE(startup); i++)
		ret = i2c_smbus_write_byte_data(f->actuator, startup[i][0], startup[i][1]);
	usleep_range(5000, 6000);
	for (i = 0; !ret && i < ARRAY_SIZE(mode); i++)
		ret = i2c_smbus_write_byte_data(f->actuator, mode[i][0], mode[i][1]);
	if (!ret) {
		ret = i2c_master_send(f->actuator, position, sizeof(position));
		ret = ret == sizeof(position) ? 0 : ret < 0 ? ret : -EIO;
	}
	if (ret)
		regulator_disable(f->vddaf);
	else
		msleep(FOCUS_SETTLE_MS);
	return ret;
}

/* Move the lens with the motor already on (preview). */
static void focus_move(struct camera *f)
{
	u8 position[3] = { FOCUS_POSITION, f->focus >> 8, f->focus & 0xff };

	if (f->actuator && !f->focus_result && i2c_master_send(f->actuator, position, sizeof(position)) == sizeof(position))
		msleep(FOCUS_SETTLE_MS);
}

static void focus_off(struct camera *f)
{
	if (f->actuator && !f->focus_result)
		regulator_disable(f->vddaf);
}

/* Look for the motor on its bus. If it is missing or does not answer, shoot without focus. */
static void focus_probe(struct camera *f)
{
	struct device *dev = &f->bus->dev;
	struct i2c_adapter *bus = NULL;
	struct device_node *np;
	struct resource r;

	f->vddaf = devm_regulator_get_optional(dev, "vddaf");
	if (IS_ERR(f->vddaf)) {
		dev_info(dev, "focus: supply not described in this kernel, lens at rest\n");
		return;
	}
	for_each_compatible_node(np, NULL, "samsung,exynosautov9-hsi2c") {
		if (of_address_to_resource(np, 0, &r) || r.start != FOCUS_BUS)
			continue;
		bus = of_get_i2c_adapter_by_node(np);
		of_node_put(np);
		break;
	}
	if (!bus) {
		dev_info(dev, "focus: the motor bus is not enabled in this kernel, lens at rest\n");
		return;
	}
	f->actuator = i2c_new_dummy_device(bus, FOCUS_ADDRESS);
	i2c_put_adapter(bus);
	if (IS_ERR(f->actuator)) {
		dev_warn(dev, "focus: motor address not available (%ld)\n", PTR_ERR(f->actuator));
		f->actuator = NULL;
		return;
	}
	/* Try it right away: better to know now that the motor does not answer than at every shot. */
	f->focus_result = focus_set(f);
	focus_off(f);
	if (f->focus_result) {
		dev_warn(dev, "focus: the motor does not answer (%d), lens at rest\n", f->focus_result);
		i2c_unregister_device(f->actuator);
		f->actuator = NULL;
	} else {
		dev_info(dev, "focus: motor ready\n");
	}
}

/* ---- Capture ---- */

/* S5KJN1: a single register, from 0 (nothing) to 4; 2 is the color bars. */
static int s5kjn1_bars(struct camera *f)
{
	return sensor_write16(f, 0x0600, f->bars);
}

/* GC08A3: first the type (0x10 = color bars), then switching on. It only has bars. */
static int gc08a3_bars(struct camera *f)
{
	if (!f->bars)
		return sensor_write8(f, 0x008c, 0);
	return sensor_write8(f, 0x008d, 0x10) ?: sensor_write8(f, 0x008c, 1);
}

static int sensor_light(struct camera *f);

/* The full-resolution mode, stopped: the sensor streams only when told to. */
static int sensor_program(struct camera *f)
{
	const struct sensor_type *t = f->type;
	int ret;

	ret = sensor_table(f, t->init, t->n_init);
	ret = ret ?: sensor_table(f, t->mode, t->n_mode);
	ret = ret ?: sensor_table(f, t->fixup, t->n_fixup);
	ret = ret ?: sensor_light(f);
	ret = ret ?: t->bars(f);
	ret = ret ?: sensor_read16(f, REG_WIDTH, &f->width_read);
	ret = ret ?: sensor_read16(f, REG_HEIGHT, &f->height_read);
	return ret;
}

/* A clock taken from its provider, without going through the device tree of our node. */
static struct clk *clock_get(const char *provider, unsigned int id)
{
	struct of_phandle_args spec = { .args_count = 1, .args = { id } };
	struct clk *clk;

	spec.np = of_find_compatible_node(NULL, NULL, provider);
	if (!spec.np)
		return ERR_PTR(-ENODEV);
	clk = of_clk_get_from_provider(&spec);
	of_node_put(spec.np);
	return clk;
}

/*
 * Bring the block clock to the working frequency or back to what it was. The register clock is
 * a fraction of it and must never exceed 100 MHz: it is slowed down before speeding up.
 */
static int block_clock(struct camera *f, bool fast)
{
	struct clk *block = clock_get("samsung,exynos850-cmu-top", CLK_DOUT_IS_BUS);
	struct clk *regs = clock_get("samsung,exynos850-cmu-is", CLK_DOUT_IS_BUSP);
	int ret = 0;

	if (IS_ERR(block) || IS_ERR(regs)) {
		ret = -ENODEV;
		goto out;
	}
	if (fast) {
		f->block_hz_before = clk_get_rate(block);
		ret = clk_set_rate(regs, f->block_hz_before / 4);
		ret = ret ?: clk_set_rate(block, BLOCK_HZ);
		ret = ret ?: clk_set_rate(regs, REGS_HZ);
		f->block_hz = clk_get_rate(block);
		f->regs_hz = clk_get_rate(regs);
	} else if (f->block_hz_before) {
		ret = clk_set_rate(block, f->block_hz_before);
		ret = ret ?: clk_set_rate(regs, f->block_hz_before / 2);
	}
out:
	if (!IS_ERR(block))
		clk_put(block);
	if (!IS_ERR(regs))
		clk_put(regs);
	return ret;
}

/* Stop the memory output and say whether it really stopped. */
static bool dma_stop(void __iomem *dma)
{
	u32 v;

	writel(0, dma + DMA_CTRL);
	writel(0, dma + DMA_FCNTSEQ);
	writel(readl(dma + DMA0_COMMON + DMA_INT_SRC), dma + DMA0_COMMON + DMA_INT_SRC);
	writel(1, dma + DMA0_COMMON + DMA_CMN_CTRL);
	return !readl_poll_timeout(dma + DMA0_COMMON + DMA_INT_SRC, v, v & DMA_STOPPED, 1000, 250000);
}

/* The state of the block and the registers needed at every shot, read once. */
static int capture_prepare(struct camera *f)
{
	const struct sensor_type *t = f->type;
	struct device *dev = &f->bus->dev;
	int i, ret;

	f->pmu = exynos_get_pmu_regmap();
	if (IS_ERR(f->pmu))
		return dev_err_probe(dev, PTR_ERR(f->pmu), "PMU not available\n");
	/* CMU_IS is read only if the block is powered: when off, the read would hang the kernel. */
	regmap_read(f->pmu, PMU_IS_STATUS, &f->is_powered);
	if (!(f->is_powered & 1))
		return dev_err_probe(dev, -ENODEV, "camera block not powered (status %#x): no shots\n",
				     f->is_powered);
	f->cmu_is = devm_ioremap(dev, CMU_IS, CMU_IS_SIZE);
	f->csis = devm_ioremap(dev, t->csis, SZ_4K);
	f->phy = devm_ioremap(dev, t->phy, DPHY_SIZE);
	f->common = devm_ioremap(dev, DMA_COMMON, SZ_4K);
	f->dma = devm_ioremap(dev, DMA0, SZ_4K);
	f->sysreg = devm_ioremap(dev, SYSREG_IS, SZ_4K);
	f->mmu = devm_ioremap(dev, SYSMMU_IS0, SYSMMU_SIZE);
	if (!f->cmu_is || !f->csis || !f->phy || !f->common || !f->dma || !f->sysreg || !f->mmu)
		return -ENOMEM;
	for (i = 0; i < ARRAY_SIZE(t->gates); i++)
		f->is_gates[i] = readl(f->cmu_is + t->gates[i]);
	for (i = 0; i < ARRAY_SIZE(is_common); i++)
		f->is_before[i] = readl(f->cmu_is + is_common[i].reg);
	ret = of_reserved_mem_device_init(dev);
	if (ret)
		return dev_err_probe(dev, ret, "memory reserved for frames missing from the device tree\n");
	ret = dma_coerce_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		of_reserved_mem_device_release(dev);
	return ret;
}

static void capture_stop(struct camera *f);

/* The light settings, written to the sensor: before streaming, or while it streams. */
static int sensor_light(struct camera *f)
{
	const struct sensor_type *t = f->type;
	unsigned int even = t->even_rows ? ~1U : ~0U;
	int ret;

	/* An exposure longer than the frame stretches it: more total rows, fewer frames per second. */
	f->exposure_used = clamp_t(unsigned int, f->exposure, t->min_exposure,
				   t->max_rows - ROW_MARGIN) & even;
	f->gain_used = clamp_t(unsigned int, f->gain, 1, t->max_gain);
	f->total_rows_used = max_t(unsigned int, t->total_rows, f->exposure_used + ROW_MARGIN) & even;
	f->frame_ms = f->total_rows_used * t->total_cols / t->pixel_khz + 1;
	ret = sensor_write16(f, REG_TOTAL_ROWS, f->total_rows_used);
	ret = ret ?: sensor_write16(f, REG_EXPOSURE, f->exposure_used);
	return ret ?: sensor_write16(f, REG_GAIN, f->gain_used * t->gain_scale);
}

/*
 * Power the sensor, receiver and memory output on and let them run: from here frames arrive one
 * after the other in the same buffer. Call with f->shot_lock held.
 */
static int capture_start(struct camera *f)
{
	const struct sensor_type *t = f->type;
	struct device *dev = &f->bus->dev;
	struct regmap *pmu = f->pmu;
	void __iomem *csis = f->csis, *phy = f->phy, *common = f->common, *dma = f->dma, *sysreg = f->sysreg;
	u32 v;
	size_t i;
	int ret;

	f->capture_phase = "checks";
	if (!dma_idle)
		return dev_err_probe(dev, -EBUSY, "the memory output had not stopped: no more shots until reboot\n");
	if (!(readl(f->cmu_is + CMU_IS_SYSMMU) & IS_GATE_ON))
		return dev_err_probe(dev, -EIO, "sysmmu clock closed: cannot tell whether it is off\n");
	for (i = 0; i < 2; i++)
		if (!(readl(f->cmu_is + t->gates[i]) & IS_GATE_ON))
			return dev_err_probe(dev, -EIO, "clock closed: %s\n", gate_names[i]);
	for (i = 0; i < ARRAY_SIZE(is_common); i++)
		if (is_common[i].gate && !(readl(f->cmu_is + is_common[i].reg) & IS_GATE_ON))
			return dev_err_probe(dev, -EIO, "clock closed: %s\n", is_common[i].name);
	f->mmu_ctrl = readl(f->mmu + MMU_CTRL);
	f->mmu_vm = readl(f->mmu + MMU_CTRL_VM);
	if ((f->mmu_ctrl & 1) || (f->mmu_vm & 1))
		return dev_err_probe(dev, -EBUSY, "sysmmu is on (%#x, %#x): a physical address would not pass\n",
				     f->mmu_ctrl, f->mmu_vm);
	/* The "not written" mark at the head of every row: marking all the memory, which bypasses the cache, would cost seconds. */
	for (i = 0; i < t->height; i++)
		memset(f->frame + i * f->row_bytes, EMPTY_FILL, ROW_HEAD);
	memset(f->frame + f->frame_bytes, GUARD_FILL, f->guard_bytes);
	f->rows_written = f->guard_touched = f->guard_touched_end = 0;

	f->capture_phase = "sensor";
	ret = sensor_power_on(f);
	if (ret)
		return ret;
	ret = f->id_result ?: sensor_program(f);
	if (ret) {
		dev_err(dev, "sensor not programmed (%d)\n", ret);
		sensor_power_off(f);
		return ret;
	}
	/* A lens that does not move does not stop the shot: it is only less sharp. */
	f->focus_result = focus_set(f);
	if (f->focus_result)
		dev_warn(dev, "focus failed (%d)\n", f->focus_result);
	if (f->width_read != t->width || f->height_read != t->height) {
		dev_err(dev, "the sensor reports %u x %u, not %u x %u: not switching on the memory output\n",
			f->width_read, f->height_read, t->width, t->height);
		ret = -EINVAL;
		goto off_sensor;
	}

	f->capture_phase = "block clock";
	ret = block_clock(f, true);
	if (ret || f->block_hz < BLOCK_HZ / 2 || f->regs_hz > REGS_HZ) {
		dev_err(dev, "block clock not set (%d): %lu Hz, registers %lu Hz\n", ret, f->block_hz,
			f->regs_hz);
		ret = ret ?: -EIO;
		goto slow_down;
	}

	f->capture_phase = "receiver";
	regmap_read(pmu, PMU_PHY, &f->phy_before);
	regmap_update_bits(pmu, PMU_PHY, 1, 1);
	/* The memory output is written at every shot: the other camera wants it different. */
	reg_update(sysreg + t->glue_mask, BIT(0), BIT(0));
	reg_update(sysreg + GLUE_OUTPUT0, GENMASK(1, 0), t->glue_output0);
	reg_update(csis + CSIS_CMN_CTRL, CSIS_SW_RESET, CSIS_SW_RESET);
	ret = readl_poll_timeout(csis + CSIS_CMN_CTRL, v, !(v & CSIS_SW_RESET), 10, 10000);
	if (ret) {
		dev_err(dev, "the receiver does not come out of reset (%#x)\n", v);
		goto off_phy;
	}
	/* D-PHY: configured in reset, below 1500 Mbit/s without alignment calibration. */
	reg_update(sysreg + PHY_RESET, t->phy_reset, 0);
	writel(2, phy + DPHY_CLOCK);
	for (i = 0; i < 4; i++) {
		reg_update(phy + DPHY_LANE(i, DPHY_ALIGN), GENMASK(6, 5), 0);
		reg_update(phy + DPHY_LANE(i, DPHY_CALIBRATE), BIT(0), 0);
		reg_update(phy + DPHY_LANE(i, DPHY_MODE), BIT(2), BIT(2));
		reg_update(phy + DPHY_LANE(i, DPHY_SETTLE), GENMASK(7, 0), t->settle);
	}
	reg_update(sysreg + PHY_RESET, t->phy_reset, t->phy_reset);
	f->phy_read[0] = readl(phy + DPHY_CLOCK);
	f->phy_read[1] = readl(phy + DPHY_LANE(0, DPHY_ALIGN));
	f->phy_read[2] = readl(phy + DPHY_LANE(0, DPHY_MODE));
	f->phy_read[3] = readl(phy + DPHY_LANE(0, DPHY_SETTLE));
	f->phy_read[4] = readl(phy + DPHY_LANE(3, DPHY_SETTLE));
	/* Receiver: four lanes, virtual channel 0, RAW10. */
	reg_update(csis + CSIS_CMN_CTRL, CSIS_LANES | CSIS_INTERLEAVE,
		   FIELD_PREP(CSIS_LANES, 3) | FIELD_PREP(CSIS_INTERLEAVE, CSIS_PER_CHANNEL));
	reg_update(csis + CSIS_DBG_OPTION, BIT(23), BIT(23));
	writel(CSIS_RAW10_DOUBLE, csis + CSIS_ISP_CONFIG_CH0);
	writel(t->height << 16 | t->width, csis + CSIS_ISP_RESOL_CH0);
	writel(0, csis + CSIS_FRM_CNT_CH0);
	writel(readl(csis + CSIS_INT_SRC0), csis + CSIS_INT_SRC0);
	writel(readl(csis + CSIS_INT_SRC1), csis + CSIS_INT_SRC1);
	/* Memory output: the values Samsung writes in the common block, then channel 0. */
	writel(2, common + 0x00);
	reg_update(common + 0x08, BIT(1), BIT(1));
	writel(0x01f001f0, common + 0x0c);
	writel(0, common + 0x10);
	writel(1, common + 0x14);
	writel(0x20, common + 0x18);
	reg_update(dma + DMA0_COMMON + 0x00, BIT(0), 0);
	writel(0, dma + DMA0_COMMON + DMA_OTF_FORMAT);
	writel(readl(dma + DMA0_COMMON + DMA_INT_SRC), dma + DMA0_COMMON + DMA_INT_SRC);
	writel(DMA_FMT_PACKED, dma + DMA_FMT);
	writel(t->width, dma + DMA_RESOL);
	writel(0, dma + DMA_FCNTSEQ);
	writel(lower_32_bits(f->frame_phys), dma + DMA_ADDR1);
	if (upper_32_bits(f->frame_phys) || readl(dma + DMA_ADDR1) != lower_32_bits(f->frame_phys)) {
		dev_err(dev, "buffer address not accepted by the memory output (%#x)\n", readl(dma + DMA_ADDR1));
		ret = -EIO;
		goto off_phy;
	}

	f->capture_phase = "frame";
	dma_idle = false;
	writel(1, dma + DMA_FCNTSEQ);
	writel(3, dma + DMA_CTRL);
	writel(0xf, csis + CSIS_UPD_SDW);
	reg_update(csis + CSIS_PHY_CMN_CTRL, GENMASK(4, 0), GENMASK(4, 0));
	reg_update(csis + CSIS_CMN_CTRL, CSIS_PHY_SEL | CSIS_QCHANNEL_EN | CSIS_EN, CSIS_EN);
	f->c_before0 = readl(csis + CSIS_INT_SRC0);
	f->c_before1 = readl(csis + CSIS_INT_SRC1);
	ret = sensor_write8(f, REG_STREAM, 1);
	if (ret) {
		dev_err(dev, "the sensor does not start (%d)\n", ret);
		f->on = true;		/* receiver and output are on: they must be turned off in order */
		capture_stop(f);
		return ret;
	}
	f->on = true;
	f->capture_phase = "streaming";
	return 0;

off_phy:
	regmap_update_bits(pmu, PMU_PHY, 1, f->phy_before & 1);
slow_down:
	block_clock(f, false);
off_sensor:
	focus_off(f);
	sensor_power_off(f);
	return ret;
}

/*
 * Wait for the end of several frames. At each one note the state and clear it: the output
 * rewrites the same buffer. After the last one the buffer holds a whole frame, until the next
 * one arrives.
 */
static void capture_wait(struct camera *f, unsigned int count)
{
	void __iomem *csis = f->csis, *dma = f->dma;
	unsigned int i;
	u32 v;

	for (i = 0; i < count; i++) {
		int slot = min_t(unsigned int, FRAMES - 1, i + FRAMES - count);

		f->capture_result[slot] = readl_poll_timeout(dma + DMA0_COMMON + DMA_INT_SRC, v, v & DMA_FRAME_END,
							     2000, (2 * f->frame_ms + 500) * 1000);
		f->c_dma[slot] = v;
		f->c_int0[slot] = readl(csis + CSIS_INT_SRC0);
		f->c_int1[slot] = readl(csis + CSIS_INT_SRC1);
		if (i < count - 1) {
			writel(v, dma + DMA0_COMMON + DMA_INT_SRC);
			writel(f->c_int0[slot], csis + CSIS_INT_SRC0);
			writel(f->c_int1[slot], csis + CSIS_INT_SRC1);
		}
	}
}

/* Power everything off, in order: the sender, the receiver, the one writing to memory. With f->shot_lock held. */
static void capture_stop(struct camera *f)
{
	void __iomem *csis = f->csis, *dma = f->dma;
	size_t i;

	if (!f->on)
		return;
	/* The error detail per channel (0x600-0x63c), the active configuration of channel 0
	 * (0x240-0x25c) and that of the memory output. */
	for (i = 0; i < ARRAY_SIZE(f->detail); i++)
		f->detail[i] = readl(csis + 0x600 + 4 * i);
	for (i = 0; i < ARRAY_SIZE(f->shadow); i++)
		f->shadow[i] = readl(csis + 0x240 + 4 * i);
	f->dma_active[0] = readl(dma + 0x30);
	f->dma_active[1] = readl(dma + 0x34);
	f->dma_active[2] = readl(dma + 0x40);
	f->dma_active[3] = readl(dma + 0x54);
	f->c_phy = readl(csis + CSIS_PHY_STATUS);
	f->c_frames = readl(csis + CSIS_FRM_CNT_CH0);
	f->c_dma_err = readl(dma + DMA0_COMMON + DMA_ERR_CODE);
	f->c_dma_state = readl(dma + DMA_ACT_CTRL);

	f->capture_phase = "stopping";
	sensor_write8(f, REG_STREAM, 0);
	msleep(f->frame_ms + 10);
	reg_update(csis + CSIS_PHY_CMN_CTRL, GENMASK(4, 0), 0);
	reg_update(csis + CSIS_CMN_CTRL, CSIS_EN | CSIS_QCHANNEL_EN, CSIS_QCHANNEL_EN);
	dma_idle = dma_stop(dma);
	if (!dma_idle)
		dev_warn(&f->bus->dev, "the memory output does not confirm the stop: the buffer will not be freed\n");
	reg_update(csis + CSIS_CMN_CTRL, CSIS_SW_RESET, CSIS_SW_RESET);
	regmap_update_bits(f->pmu, PMU_PHY, 1, f->phy_before & 1);
	block_clock(f, false);
	focus_off(f);
	sensor_power_off(f);
	f->on = false;
	f->capture_phase = "off";
}

/*
 * A shot: the frame stays in f->frame. From off, it powers on, lets the first frames go by and
 * powers off; from on (preview), it waits for two, because the incoming one may still have the
 * previous settings, and powers off. Call with f->shot_lock held.
 */
static int capture_shoot(struct camera *f)
{
	const struct sensor_type *t = f->type;
	struct device *dev = &f->bus->dev;
	bool was_on = f->on;
	u8 *b;
	size_t i;
	int ret;

	if (!was_on) {
		ret = capture_start(f);
		if (ret)
			return ret;
	}
	/* The state left by the previous end must be cleared, or it would be taken as good. */
	writel(readl(f->dma + DMA0_COMMON + DMA_INT_SRC), f->dma + DMA0_COMMON + DMA_INT_SRC);
	capture_wait(f, was_on ? 2 : FRAMES);
	capture_stop(f);

	b = f->frame;
	/* A row counts as written if something changed at its head. */
	for (i = 0; i < t->height; i++)
		f->rows_written += memchr_inv(b + i * f->row_bytes, EMPTY_FILL, ROW_HEAD) != NULL;
	for (i = 0; i < f->guard_bytes; i++) {
		if (b[f->frame_bytes + i] != GUARD_FILL) {
			f->guard_touched++;
			f->guard_touched_end = i + 1;
		}
	}
	f->capture_phase = "done";
	if (f->capture_result[FRAMES - 1] || f->rows_written != t->height || f->guard_touched) {
		dev_err(dev, "shot failed: last frame %s, %zu rows written out of %u, %zu bytes beyond the frame\n",
			f->capture_result[FRAMES - 1] ? "NOT arrived" : "arrived", f->rows_written, t->height,
			f->guard_touched);
		return -EIO;
	}
	return 0;
}

/* ---- The device ---- */

/*
 * Only one at a time, both cameras included: whoever reads in pieces must not find a shot taken
 * by another under them, and the cameras share the memory output and the clock. The frame memory
 * is taken here and returned on close.
 */
static int camera_open(struct inode *inode, struct file *file)
{
	struct camera *f = container_of(file->private_data, struct camera, miscdev);

	if (test_and_set_bit(0, &busy))
		return -EBUSY;
	/* Left over from a shot where the memory output did not stop: that one is reused. */
	if (!f->frame)
		f->frame = dma_alloc_coherent(&f->bus->dev, f->frame_bytes + f->guard_bytes, &f->frame_phys, GFP_KERNEL);
	if (!f->frame) {
		clear_bit(0, &busy);
		return -ENOMEM;
	}
	f->shots_here = 0;
	return 0;
}

static int camera_release(struct inode *inode, struct file *file)
{
	struct camera *f = container_of(file->private_data, struct camera, miscdev);

	mutex_lock(&f->shot_lock);
	capture_stop(f);		/* a preview left on */
	/* A memory output that has not stopped would still write there: in that case the buffer stays. */
	if (dma_idle) {
		dma_free_coherent(&f->bus->dev, f->frame_bytes + f->guard_bytes, f->frame, f->frame_phys);
		f->frame = NULL;
	}
	mutex_unlock(&f->shot_lock);
	clear_bit(0, &busy);
	return 0;
}

static ssize_t camera_read(struct file *file, char __user *buf, size_t n, loff_t *pos)
{
	struct camera *f = container_of(file->private_data, struct camera, miscdev);
	ssize_t ret;

	if (mutex_lock_interruptible(&f->shot_lock))
		return -ERESTARTSYS;
	if (f->on)
		ret = simple_read_from_buffer(buf, n, pos, f->frame, f->frame_bytes);	/* the preview */
	else if (!f->shots_here)
		ret = -ENODATA;		/* the memory was just taken: there is no photo in it */
	else if (f->shot_result)
		ret = f->shot_result;
	else
		ret = simple_read_from_buffer(buf, n, pos, f->frame, f->frame_bytes);
	mutex_unlock(&f->shot_lock);
	return ret;
}

static ssize_t camera_write(struct file *file, const char __user *from, size_t n, loff_t *pos)
{
	struct camera *f = container_of(file->private_data, struct camera, miscdev);
	const struct sensor_type *t = f->type;
	char line[96] = {}, *rest = line, *word;
	bool light = false, focus = false;
	unsigned int value;
	ssize_t ret = n;

	if (n >= sizeof(line))
		return -EINVAL;
	if (copy_from_user(line, from, n))
		return -EFAULT;
	if (mutex_lock_interruptible(&f->shot_lock))
		return -ERESTARTSYS;
	while (ret > 0 && (word = strsep(&rest, " \n")) != NULL) {
		if (!*word)
			continue;
		if (sscanf(word, "exposure=%u", &value) == 1 && value >= t->min_exposure &&
		    value <= t->max_rows - ROW_MARGIN) {
			f->exposure = value;
			light = true;
		} else if (sscanf(word, "gain=%u", &value) == 1 && value >= 1 && value <= t->max_gain) {
			f->gain = value;
			light = true;
		} else if (sscanf(word, "bars=%u", &value) == 1 && value <= 4) {
			f->bars = value;
		} else if (sscanf(word, "focus=%u", &value) == 1 && value <= FOCUS_MAX) {
			f->focus = value;
			focus = true;
		} else if (!strcmp(word, "start")) {
			if (!f->on) {
				ret = capture_start(f) ?: ret;
				light = focus = false;	/* just written by the power-up */
			}
		} else if (!strcmp(word, "shoot")) {
			/* While on, light and focus changed in this same line apply to the shot. */
			if (f->on && light)
				sensor_light(f);
			if (f->on && focus)
				focus_move(f);
			light = focus = false;
			f->shots++;
			f->shots_here++;
			f->shot_result = capture_shoot(f);
			if (f->shot_result)
				ret = f->shot_result;
		} else {
			ret = -EINVAL;
		}
	}
	/* With the preview on, the new settings apply immediately. */
	if (ret > 0 && f->on && light)
		ret = sensor_light(f) ?: ret;
	if (ret > 0 && f->on && focus)
		focus_move(f);
	mutex_unlock(&f->shot_lock);
	return ret;
}

static const struct file_operations camera_fops = {
	.owner = THIS_MODULE,
	.open = camera_open,
	.release = camera_release,
	.read = camera_read,
	.write = camera_write,
	.llseek = default_llseek,
};

static int status_show(struct seq_file *s, void *v)
{
	struct camera *f = s->private;
	const struct sensor_type *t = f->type;
	int i;

	seq_printf(s, "sensor: %s, %u x %u, row of %zu bytes, frame of %zu bytes\n", t->sensor, t->width,
		   t->height, f->row_bytes, f->frame_bytes);
	seq_printf(s, "camera block: %spowered (PMU 0x2104 = %#x)\n",
		   f->is_powered & 1 ? "" : "NOT ", f->is_powered);
	for (i = 0; i < ARRAY_SIZE(t->gates); i++)
		seq_printf(s, "CMU_IS %#06x = %#x  %s\n", t->gates[i], f->is_gates[i], gate_names[i]);
	for (i = 0; i < ARRAY_SIZE(is_common); i++)
		seq_printf(s, "CMU_IS %#06x = %#x  %s\n", is_common[i].reg, f->is_before[i], is_common[i].name);
	seq_printf(s, "next shot: exposure %u rows, gain %u, test pattern %u, focus %u\n",
		   f->exposure, f->gain, f->bars, f->focus);
	if (f->actuator)
		seq_printf(s, "focus: motor present, last result %d\n", f->focus_result);
	else
		seq_puts(s, "focus: absent (fixed lens or at rest)\n");
	if (!f->shots) {
		seq_puts(s, "shots: none so far\n");
	} else {
		seq_printf(s, "shots: %u; the last: result %d, exposure %u rows out of %u total, gain %u\n", f->shots,
			   f->shot_result, f->exposure_used, f->total_rows_used, f->gain_used);
		seq_printf(s, "sensor writes retried so far: %u\n", f->retried_writes);
		seq_printf(s, "capture: phase \"%s\", buffer at %pad, sysmmu %#x %#x\n", f->capture_phase, &f->frame_phys,
			   f->mmu_ctrl, f->mmu_vm);
		seq_printf(s, "capture: the sensor reports %u x %u\n", f->width_read, f->height_read);
		seq_printf(s, "capture: D-PHY read back: clock %#x, alignment %#x, mode %#x, lock %#x and %#x\n",
			   f->phy_read[0], f->phy_read[1], f->phy_read[2], f->phy_read[3], f->phy_read[4]);
		seq_printf(s, "capture: block clock %lu Hz (before %lu), registers %lu Hz\n", f->block_hz,
			   f->block_hz_before, f->regs_hz);
		seq_printf(s, "capture: receiver before start: errors %#x, events %#x\n", f->c_before0, f->c_before1);
		for (i = 0; i < FRAMES; i++)
			seq_printf(s, "capture: frame %d: %s; memory output %#x; receiver: errors %#x, events %#x\n",
				   i + 1, f->capture_result[i] ? "end NOT arrived" : "end arrived", f->c_dma[i],
				   f->c_int0[i], f->c_int1[i]);
		seq_puts(s, "capture: error detail 0x600..:");
		for (i = 0; i < ARRAY_SIZE(f->detail); i++)
			seq_printf(s, " %x", f->detail[i]);
		seq_puts(s, "\ncapture: active configuration 0x240..:");
		for (i = 0; i < ARRAY_SIZE(f->shadow); i++)
			seq_printf(s, " %x", f->shadow[i]);
		seq_printf(s, "\ncapture: memory output active: control %#x, format %#x, width %#x, sequence %#x\n",
			   f->dma_active[0], f->dma_active[1], f->dma_active[2], f->dma_active[3]);
		seq_printf(s, "capture: lanes %#x, frames counted %u, memory output: errors %#x, active %#x\n",
			   f->c_phy, f->c_frames, f->c_dma_err, f->c_dma_state);
		seq_printf(s, "capture: %zu rows written out of %u; beyond the frame %zu bytes, up to %zu (%zu rows) out of %zu of guard\n",
			   f->rows_written, t->height, f->guard_touched, f->guard_touched_end,
			   f->guard_touched_end / f->row_bytes, f->guard_bytes);
		if (f->id_result)
			seq_printf(s, "sensor: does not answer on I2C (%d)\n", f->id_result);
		else
			seq_printf(s, "sensor: identifier %#06x (expected %#06x)\n", f->id, t->id);
	}
	for (i = 0; i < ARRAY_SIZE(t->clock); i++)
		seq_printf(s, "sensor clock, %s %#06x: at load %#x\n", clock_names[i], t->clock[i],
			   f->before[i]);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(status);

static int camera_probe(struct i2c_client *bus)
{
	struct device *dev = &bus->dev;
	const struct sensor_type *t = of_device_get_match_data(dev);
	struct camera *f;
	int i, ret;

	f = devm_kzalloc(dev, sizeof(*f), GFP_KERNEL);
	if (!f)
		return -ENOMEM;
	f->type = t;
	f->bus = bus;
	i2c_set_clientdata(bus, f);
	f->row_bytes = ALIGN(t->width * 12 / 8, 16);
	f->frame_bytes = f->row_bytes * t->height;
	f->guard_bytes = f->row_bytes * GUARD_ROWS;
	f->vddio = devm_regulator_get(dev, "vddio");
	f->vddd = devm_regulator_get(dev, "vddd");
	f->vdda = devm_regulator_get(dev, "vdda");
	if (IS_ERR(f->vddio) || IS_ERR(f->vddd) || IS_ERR(f->vdda))
		return dev_err_probe(dev, PTR_ERR(IS_ERR(f->vddio) ? f->vddio : IS_ERR(f->vddd) ? f->vddd : f->vdda),
				     "supplies not ready (module s2mpb03-regulator)\n");
	/* Reset held from the start: the sensor must not start before the supplies. */
	f->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(f->reset))
		return dev_err_probe(dev, PTR_ERR(f->reset), "reset pin not available\n");
	f->select = devm_gpiod_get_optional(dev, "select", GPIOD_OUT_LOW);
	if (IS_ERR(f->select))
		return dev_err_probe(dev, PTR_ERR(f->select), "select pin not available\n");
	f->cmu = devm_ioremap(dev, CMU_TOP, CMU_TOP_SIZE);
	if (!f->cmu)
		return -ENOMEM;
	for (i = 0; i < ARRAY_SIZE(t->clock); i++)
		f->before[i] = readl(f->cmu + t->clock[i]);

	ret = capture_prepare(f);
	if (ret)
		return ret;
	mutex_init(&f->shot_lock);
	if (t->has_focus)
		focus_probe(f);
	f->exposure = t->total_rows - ROW_MARGIN;	/* a whole frame: 33 ms */
	f->gain = 4;
	f->status = debugfs_create_file(t->name, 0400, NULL, f, &status_fops);
	f->miscdev.minor = MISC_DYNAMIC_MINOR;
	f->miscdev.name = t->name;
	f->miscdev.fops = &camera_fops;
	f->miscdev.mode = 0600;
	ret = misc_register(&f->miscdev);
	if (ret) {
		debugfs_remove(f->status);
		if (f->actuator)
			i2c_unregister_device(f->actuator);
		of_reserved_mem_device_release(dev);
		return dev_err_probe(dev, ret, "/dev/%s not created\n", t->name);
	}
	return 0;
}

static void camera_remove(struct i2c_client *bus)
{
	struct camera *f = i2c_get_clientdata(bus);

	misc_deregister(&f->miscdev);
	debugfs_remove(f->status);
	if (f->actuator)
		i2c_unregister_device(f->actuator);
	of_reserved_mem_device_release(&bus->dev);
}

static const struct sensor_type rear = {
	.name = "camera-rear",
	.sensor = "S5KJN1",
	.clock = { 0x1008, 0x1814, 0x2010, 0x3004 },		/* CIS_CLK0 */
	.power_on_ms = 15,
	.id_reg = 0x0000,
	.id = 0x38e1,
	.init = s5kjn1_init,
	.n_init = ARRAY_SIZE(s5kjn1_init),
	.mode = s5kjn1_4080x3072,
	.n_mode = ARRAY_SIZE(s5kjn1_4080x3072),
	.fixup = s5kjn1_26mhz,
	.n_fixup = ARRAY_SIZE(s5kjn1_26mhz),
	.bars = s5kjn1_bars,
	.width = 4080,
	.height = 3072,
	/* 5200 columns by 3565 rows at 563 MHz: 9.2 us per row and 33 ms per frame. */
	.total_cols = 5200,
	.total_rows = 3565,
	.max_rows = 0xffff,
	.pixel_khz = 563333,
	.min_exposure = 8,
	.max_gain = 64,
	.gain_scale = 32,
	.has_focus = true,
	.csis = 0x14410000,					/* CSIS1 */
	.phy = 0x144a0000,
	.phy_reset = BIT(1),
	.glue_mask = 0x0440,
	.glue_output0 = 1,
	.settle = 71,						/* 1443 Mbit/s per lane */
	.gates = { 0x2008, 0x2044, 0x3010 },
};

static const struct sensor_type front = {
	.name = "camera-front",
	.sensor = "GC08A3",
	.clock = { 0x100c, 0x1818, 0x2014, 0x3008 },		/* CIS_CLK1 */
	.reset_before_clock = true,
	.supply_delay_us = { 1000, 100, 100 },
	.power_on_ms = 5,
	.id_reg = 0x03f0,
	.id = 0x08a3,
	.data8 = true,
	.init = gc08a3_init,
	.n_init = ARRAY_SIZE(gc08a3_init),
	.mode = gc08a3_3264x2448,
	.n_mode = ARRAY_SIZE(gc08a3_3264x2448),
	.fixup = gc08a3_26mhz,
	.n_fixup = ARRAY_SIZE(gc08a3_26mhz),
	.bars = gc08a3_bars,
	.width = 3264,
	.height = 2448,
	/* 3672 columns by 2548 rows at 280.8 MHz: 13.1 us per row and 33 ms per frame. */
	.total_cols = 3672,
	.total_rows = 2548,
	.max_rows = 0xfff0,
	.pixel_khz = 280800,
	.min_exposure = 16,
	.even_rows = true,
	.max_gain = 16,
	.gain_scale = 1024,
	.csis = 0x14400000,					/* CSIS0 */
	.phy = 0x14490000,
	.phy_reset = BIT(0),
	.glue_mask = 0x0438,
	.glue_output0 = 0,
	.settle = 34,						/* 669.5 Mbit/s per lane */
	.gates = { 0x2004, 0x2040, 0x300c },
};

static const struct of_device_id camera_of[] = {
	{ .compatible = "mangos,s5kjn1", .data = &rear },
	{ .compatible = "mangos,gc08a3", .data = &front },
	{ }
};
MODULE_DEVICE_TABLE(of, camera_of);

static struct i2c_driver camera_driver = {
	.driver = { .name = "camera-mangos", .of_match_table = camera_of },
	.probe = camera_probe,
	.remove = camera_remove,
};
module_i2c_driver(camera_driver);

MODULE_DESCRIPTION("M13 cameras: raw shots from /dev/camera-rear and /dev/camera-front");
MODULE_LICENSE("GPL");
