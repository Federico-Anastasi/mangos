// SPDX-License-Identifier: GPL-2.0-only
// Portions Copyright (c) Samsung Electronics Co., Ltd.
/*
 * Sensor microcontroller (CHUB) of the M13: starts it with the original Samsung firmware,
 * exchanges messages with it and reads the accelerometer (LIS2DLC12) and the compass (MMC5633),
 * which are wired to the CHUB and not to the CPU.
 *
 * On load, in order: sensor power and clocks; address window (BAAW) through which the CHUB
 * reaches its buses; bootloader and firmware copied into the CHUB RAM and read back; reset
 * release; wait for the "alive" signal; version and sensor list requested from the firmware;
 * mounting position of the sensors and switching them on. From there a periodic work drains the
 * message queues and the firmware log; messages arrive on the mailbox interrupt, and if it cannot
 * be requested the periodic work reads them at intervals.
 *
 * The CHUB block and the sensor clocks are left on by the bootloader (clk_ignore_unused and
 * pd_ignore_unused in the boot arguments): the module never turns them off. On exit the CHUB
 * processor goes back into reset and the block stays powered, as at phone boot.
 *
 * In /sys/kernel/debug/chub:
 *   status    phase reached, version, PMU and mailbox registers, queues, last sample
 *   log       text: firmware log, messages sent and received
 *   command   "accel 1 [ms]" turns the accelerometer on (period in milliseconds), "accel 0"
 *             turns it off, "compass 1 [ms]" and "compass 0" the same for the compass;
 *             "cmd C T S [bytes in hexadecimal]" sends a command with the header built here
 *             (C command, T sensor, S subcommand); "msg bytes..." sends a raw packet
 *
 * Registers, values and order come from the Samsung M135F sources: drivers/staging/nanohub/
 * (chub.c, chub_ipc.c, chub_ipc.h, chub.h), drivers/sensorhub/ (sensor protocol),
 * drivers/soc/samsung/cal-if/exynos3830/pmucal_chub_exynos3830.h (PMU sequences) and from the
 * "contexthub" node of the original device tree.
 *
 * Load it with modprobe chub; /lib/firmware needs shub_nacho_bl.bin and shub_nacho_m13.bin.
 */
#include <linux/arm-smccc.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/hex.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/sched/clock.h>
#include <linux/seq_file.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/soc/samsung/exynos-pmu.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/unaligned.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <dt-bindings/clock/exynos850.h>
#include <dt-bindings/interrupt-controller/arm-gic.h>

/* Sensor power: LDO30 of the power chip. */
#define REGULATOR_NAME		"vdd_sensor_1p8"
/* Firmware log level: 0 off, 1 and 2 on (Samsung uses 2). */
#define LOG_LEVEL		2
/* Period with which accelerometer and compass are switched on at start, in milliseconds. */
#define ACCEL_PERIOD_MS		100
#define COMPASS_PERIOD_MS	100
/* Accelerometer mounting position: "acc-lis2dlc12-position" of the M13 device tree. */
#define ACCEL_POSITION		8
/*
 * Compass mounting position and correction matrix: "mag-mmc5633-position" and
 * "mag-mmc5633-array" of the M13 device tree (the one for phones without an NFC antenna).
 */
#define COMPASS_POSITION	3
static const u8 compass_matrix[27] = {
	0x23, 0xfa, 0x01, 0x5e, 0x00, 0xdc, 0x00, 0xfa, 0x25, 0xb2, 0x00, 0x14, 0x00, 0xbe,
	0x00, 0x1e, 0x29, 0x04,
};

/* "contexthub" node of the original device tree. */
#define MAILBOX_BASE		0x11a00000
#define MAILBOX_SIZE		0x200
#define SRAM_BASE		0x10e00000
#define SRAM_SIZE		0x40000
#define DUMPGPR_BASE		0x10cf0000
#define DUMPGPR_SIZE		0x100
#define BAAW_BASE		0x10d20000
#define BAAW_SIZE		0x10

/* PMU: pmucal_chub_exynos3830.h. In every register bit 0 is what counts. */
#define PMU_CHUB_CONFIGURATION	0x1f80
#define PMU_CHUB_STATUS		0x1f84
#define PMU_CHUB_1F88		0x1f88		/* only read, to compare with boot */
#define PMU_CHUB_OPTION		0x1f8c
#define PMU_CHUB_CPU_CONFIG	0x2d00
#define PMU_CHUB_CPU_STATUS	0x2d04
/* Samsung waits 5 ms; here longer, since the wait ends as soon as the bit changes. */
#define PMU_WAIT_MS		100

/* Window through which the CHUB sees APM and CMGP: "baaw,baaw-p-apm-chub" and chub.h. */
#define BAAW_START		0x0
#define BAAW_END		0x4
#define BAAW_REMAP		0x8
#define BAAW_PERMISSIONS	0xc
#define BAAW_VAL_START		0x40300
#define BAAW_VAL_END		0x40900
#define BAAW_VAL_REMAP		0x11900
#define BAAW_RW_ACCESS_ENABLE	0x80000003

/* Registers showing where the CHUB processor is running (chub.h, chub_dbg.c). */
#define DUMPGPR_CTRL		0x0
#define DUMPGPR_PCR		0x4
#define DUMPGPR_GP0R		0x10

/*
 * Mailbox (chub_ipc.h). We are the "source" side: interrupts to the CHUB are raised in INTGR1
 * (bit = number), those from the CHUB are read in INTSR0 and cleared in INTCR0 (bit = number +
 * 16).
 */
#define MBOX_INTCR0		0x00c
#define MBOX_INTMR0		0x010
#define MBOX_INTSR0		0x014
#define MBOX_INTMSR0		0x018
#define MBOX_INTGR1		0x01c
#define MBOX_INTSR1		0x028
#define MBOX_ISSR(n)		(0x080 + 4 * (n))
#define SR_BOOT_MODE		0
#define SR_CHUB_ALIVE		3
#define IRQ_DAL_CHUB		16
#define IRQ_NUM_EVT_END		14
#define IRQ_CHUB_LOG		14
#define IRQ_CHUB_ALIVE		15
#define AP_WAKE			0x1
#define CHUB_REBOOT_REQ		0xff

/* Call to the secure firmware: include/linux/smc.h and include/soc/samsung/exynos-el3_mon.h. */
#define SMC_CMD_CONN_IF		0x82000710UL
#define EXYNOS_SHUB		2UL
#define EXYNOS_SET_CONN_TZPC	0UL

/* Boot data inside the bootloader, and shared memory (chub_ipc.h). */
#define MAP_INFO_OFFSET		256
#define OS_UPDT_MAGIC		"Nanohub OS"
#define CHUB_IPC_MAGIC		"IPC magic"
#define IPC_VERSION		190128
#define BOOTMODE_COLD		0x7733
#define FW_VERSION_OFFSET	0xf0		/* shub_firmware.c */
#define PACKET_SIZE_MAX		777
#define IPC_CH_BUF_NUM		16
#define IPC_EVT_NUM		50
#define IRQ_MAX			16
#define LOGBUF_NUM		80
#define LOGBUF_DATA_SIZE	74
#define LOGBUF_TOTAL_SIZE	(84 * LOGBUF_NUM)
#define CHANNEL_INVALID		0xffff
#define IPC_EVT_DQ		0
#define IPC_EVT_EQ		1
enum { IPC_DATA_C2A, IPC_DATA_A2C };
enum { IPC_EVT_C2A, IPC_EVT_A2C };
/* Events: the first three mean "there is a packet"; then those from the CHUB to us. */
#define IRQ_EVT_CH0		0
#define IRQ_EVT_CH_MAX		3
#define IRQ_EVT_C2A_DEBUG	3
#define IRQ_EVT_C2A_ASSERT	4
#define IRQ_EVT_C2A_INT		5
#define IRQ_EVT_C2A_INTCLR	6

struct chub_bootargs {
	char magic[16];
	u32 ipc_version;
	u32 bl_start, bl_end;
	u32 code_start, code_end;
	u32 ipc_start, ipc_end;
	u32 ram_start, ram_end;
	u32 shared_start, shared_end;
	u32 dump_start, dump_end;
	u32 chubclk;
	u16 bootmode;
	u16 runtimelog;
};

struct ipc_channel {
	u32 size;
	u8 buf[PACKET_SIZE_MAX];
};

/* Packet queue: eq is moved by the writer, dq by the reader. */
struct ipc_data {
	u32 eq, dq, full, empty;
	struct ipc_channel ch[IPC_CH_BUF_NUM];
};

struct ipc_event {
	u32 evt, irq, status;
};

/* Event queue: each one is tied to a mailbox interrupt, marked in pending. */
struct ipc_events {
	struct ipc_event data[IPC_EVT_NUM];
	struct {
		u32 eq, dq, full, empty, irq;
		u8 pending[IRQ_MAX];
	} ctrl;
};

struct ipc_log_line {
	char buf[LOGBUF_DATA_SIZE];
	u8 error, newline;
	u32 size;			/* progressive number of the line */
	u32 nextaddr;
};

struct ipc_log {
	struct ipc_log_line log[LOGBUF_NUM];
	u32 eq, dq, size;
	u8 full, flush_req, flush_active, loglevel;
	s32 dbg_full_cnt, errcnt, reqcnt;
	u32 fw_num;
	struct {
		u32 eq, dq, size, full;
		char buf[];
	} raw;				/* second log, in characters */
};

/* All the shared memory (ipc_map_area), at address ipc_start of the CHUB RAM. */
struct ipc_map {
	char persist[96];
	char magic[16];
	u16 wake;
	struct ipc_data data[2];
	struct ipc_events evt[2];
	struct {
		u32 event, val[2];
	} dbg;
	struct {
		char magic[16];
		s32 index;
		u8 active[36];
	} sensors;
	struct ipc_log log;
};

/* The same sizes the firmware has: if they change, the queues no longer match. */
static_assert(sizeof(struct chub_bootargs) == 76);
static_assert(sizeof(struct ipc_channel) == 784);
static_assert(sizeof(struct ipc_events) == 636);
static_assert(sizeof(struct ipc_log_line) == 84);
static_assert(offsetof(struct ipc_map, data) == 116);
static_assert(sizeof(struct ipc_map) == 33344);

#define M(field)		offsetof(struct ipc_map, field)
#define DATA(dir, field)	(M(data) + (dir) * sizeof(struct ipc_data) + offsetof(struct ipc_data, field))
#define CHANNEL(dir, i)		(DATA(dir, ch) + (i) * sizeof(struct ipc_channel))
#define EVT(q, field)		(M(evt) + (q) * sizeof(struct ipc_events) + offsetof(struct ipc_events, field))
#define EVT_ENTRY(q, i)		(EVT(q, data) + (i) * sizeof(struct ipc_event))
#define LOG_LINE(i)		(M(log.log) + (i) * sizeof(struct ipc_log_line))

/* Sensor protocol (drivers/sensorhub/comm/shub_comm.c, shub_cmd.h, shub_sensor_type.h). */
#define SHUB_CMD_SIZE		64
enum { CMD_ADD, CMD_REMOVE, CMD_CHANGERATE, CMD_SETVALUE, CMD_GETVALUE, CMD_REPORT };
#define TYPE_MCU			255
#define SENSOR_TYPE_ACCELEROMETER	1
#define SENSOR_TYPE_GEOMAGNETIC_FIELD	2
#define SUB_RTC_TIME			4
#define SUB_LCD_ON			5
#define SUB_HUB_SYSTEM_INFO		12
#define SUB_SENSOR_AXIS			26
#define SUB_SENSOR_SPEC			27
#define SUB_MAGNETIC_STATIC_MATRIX	32
#define SUB_CAL_DATA			35
#define SUB_PM_STATE			56
#define PM_COMPLETE			4
#define SHUB2AP_LIBRARY_DATA		0x01
#define SHUB2AP_DEBUG_DATA		0x03
#define SHUB2AP_META_DATA		0x05
#define SHUB2AP_NOTI_RESET		0x07
#define SHUB2AP_REQ_RESET		0x0a
#define SHUB2AP_BYPASS_DATA		0x37
#define REPLY_MS			1000
#define TIME_SYNC_SECONDS		10	/* how often the time is given again to the firmware */

/* Header of every packet, in both directions; the time is since boot, in nanoseconds. */
struct shub_header {
	u8 cmd, type, subcmd;
	__le16 total_length, length;
	__le64 timestamp;
} __packed;

struct shub_system_info {
	__le32 fw_version;
	__le64 scan[3];			/* sensors found, one bit per type */
	__le32 support_ddi, reserved[2];
} __packed;

struct sensor_spec {
	u8 uid;
	char name[15];
	u8 vendor;
	__le32 version;
	u8 is_wake_up;
	__le32 min_delay, max_delay;
	__le16 max_fifo, reserved_fifo;
	__le32 resolution, max_range, power;	/* floating point numbers */
} __packed;

static_assert(sizeof(struct shub_header) == 15);
static_assert(sizeof(struct shub_system_info) == 40);
static_assert(sizeof(struct sensor_spec) == 46);

/* The sensors read here; the unit is the one the firmware declares with the resolution. */
enum { ACCELEROMETER, COMPASS, N_SENSORS };

struct sensor {
	u8 type;
	const char *label, *unit;
	char name[16];
	u64 res_nano;			/* resolution declared by the firmware, in billionths */
	bool on;
	u32 ms;
	/* The last sample, under sample_lock. */
	s32 x, y, z;
	u8 accuracy;			/* compass only: from 0 (not calibrated) to 3 */
	u64 time, samples;
};

#define PAYLOAD_MAX		(SHUB_CMD_SIZE - sizeof(struct shub_header))
#define LOG_SIZE		SZ_64K
#define BIG_SIZE		2048
#define POLL_MS			5
/*
 * The mailbox interrupt ("contexthub" node of the Samsung device tree, the first of the two).
 * When it is there, the periodic work stays only for the time and as a safety net.
 */
#define SPI_MAILBOX		50
#define POLL_SLOW_MS		1000
#define ALIVE_TRIES		10

/* Clocks of the CMGP block, where the sensor buses and pins are: they must stay open. */
static const unsigned int cmgp_gates[] = {
	CLK_GOUT_CMGP_GPIO_PCLK, CLK_GOUT_SYSREG_CMGP_PCLK, CLK_GOUT_CMGP_USI0_PCLK,
	CLK_GOUT_CMGP_USI0_IPCLK, CLK_GOUT_CMGP_USI1_PCLK, CLK_GOUT_CMGP_USI1_IPCLK,
};

struct chub {
	struct device *dev;
	struct regmap *pmu;
	struct regulator *vdd;
	void __iomem *mbox, *sram, *gpr, *baaw, *ipc;
	struct clk *chub_gate, *div_chub, *cmgp[ARRAY_SIZE(cmgp_gates)];
	const struct firmware *fw_bl, *fw_os;
	struct chub_bootargs map;
	u32 ipc_bytes, hz, file_version;
	bool vdd_on, gate_was_open;

	const char *phase;
	bool cpu_released, alive, alive_seen, ready, exiting, queue_broken;
	u64 release_ns;
	u32 alive_ms, fw_version;
	u64 found[3];

	struct delayed_work poll;
	int irq;			/* of the mailbox; 0 = not available, polled at intervals */
	struct mutex rx_lock;		/* interrupt and periodic work do not read together */
	unsigned int interrupts;
	wait_queue_head_t waitq;
	struct mutex tx_lock;
	unsigned int sent, received, anomalies, other_events;
	unsigned long next_time_sync;
	u8 rx[PACKET_SIZE_MAX];
	/* Replies that arrived in several packets: reassembled here. */
	u8 big[BIG_SIZE];
	u64 big_time;
	unsigned int big_total, big_len;

	/* One question at a time: the reply has the same sensor and subcommand. */
	struct mutex ask_lock, reply_lock;
	struct {
		u8 type, subcmd;
		bool waiting, arrived;
		u8 *buf;
		unsigned int max, len;
	} reply;

	struct mutex sensors_lock;
	spinlock_t sample_lock;
	struct sensor sensors[N_SENSORS];
	unsigned int samples_logged;

	struct dentry *dir;
	struct mutex log_lock;
	u64 log_written;
	char log[LOG_SIZE];
};

/* ---- Text log ---- */

static void log_append(struct chub *c, const char *text, size_t n)
{
	mutex_lock(&c->log_lock);
	for (; n; n--)
		c->log[c->log_written++ % LOG_SIZE] = *text++;
	mutex_unlock(&c->log_lock);
}

static __printf(2, 3) void note(struct chub *c, const char *fmt, ...)
{
	char line[240];
	va_list args;
	u32 rem;
	u64 s = div_u64_rem(local_clock(), NSEC_PER_SEC, &rem);
	int n = scnprintf(line, sizeof(line) - 1, "[%5llu.%03u] ", s, rem / 1000000);

	va_start(args, fmt);
	n += vscnprintf(line + n, sizeof(line) - 1 - n, fmt, args);
	va_end(args);
	line[n++] = '\n';
	log_append(c, line, n);
}

/* Like note, but only the first times: for anomalies that could repeat at every round. */
#define note_anomaly(c, fmt, ...) do {					\
	if ((c)->anomalies++ < 40)					\
		note(c, "ANOMALY: " fmt, ##__VA_ARGS__);		\
} while (0)

/* ---- Simple accessors ---- */

static u32 m_r32(struct chub *c, size_t offset)
{
	return readl(c->ipc + offset);
}

static void m_w32(struct chub *c, size_t offset, u32 value)
{
	writel(value, c->ipc + offset);
}

static struct clk *clock_get(struct device_node *cmu, unsigned int id)
{
	struct of_phandle_args spec = { .np = cmu, .args_count = 1, .args = { id } };

	return of_clk_get_from_provider(&spec);
}

static void clock_put(struct clk *clk)
{
	if (!IS_ERR_OR_NULL(clk))
		clk_put(clk);
}

static int pmu_bit(struct chub *c, u32 reg, u32 value)
{
	return regmap_update_bits(c->pmu, reg, 1, value);
}

static int pmu_wait(struct chub *c, u32 reg, u32 expected, const char *name)
{
	u32 v = 0;
	int ret = regmap_read_poll_timeout(c->pmu, reg, v, (v & 1) == expected, 100, PMU_WAIT_MS * 1000);

	if (ret)
		dev_err(c->dev, "PMU: %s (%#x) stays at %#x, expected bit 0 at %u (%d)\n", name, reg,
			v, expected, ret);
	return ret;
}

/* ---- PMU sequences: pmucal_chub_exynos3830.h ---- */

/* shub_reset_release_config: powers the block, without starting its processor. */
static int block_power_on(struct chub *c)
{
	int ret = pmu_bit(c, PMU_CHUB_CONFIGURATION, 1);

	ret = ret ?: pmu_wait(c, PMU_CHUB_STATUS, 1, "CHUB_STATUS");
	return ret ?: pmu_bit(c, PMU_CHUB_OPTION, 0);
}

/* shub_reset_assert: cuts power to the block. After this, its RAM must not be touched. */
static int block_power_off(struct chub *c)
{
	int ret;

	ret = pmu_bit(c, PMU_CHUB_OPTION, 1);
	ret = ret ?: pmu_bit(c, PMU_CHUB_CONFIGURATION, 0);
	return ret ?: pmu_wait(c, PMU_CHUB_STATUS, 0, "CHUB_STATUS");
}

/* shub_reset_release: starts the CHUB processor. */
static int cpu_release(struct chub *c)
{
	int ret = pmu_bit(c, PMU_CHUB_CPU_CONFIG, 1);

	return ret ?: pmu_wait(c, PMU_CHUB_CPU_STATUS, 1, "CHUB_CPU_STATUS");
}

/*
 * The inverse of shub_reset_release. Samsung never does it (it powers off the whole block): here
 * it leaves the CHUB as it was at phone boot, powered and with the processor in reset.
 */
static int cpu_halt(struct chub *c)
{
	int ret = pmu_bit(c, PMU_CHUB_CPU_CONFIG, 0);

	return ret ?: pmu_wait(c, PMU_CHUB_CPU_STATUS, 0, "CHUB_CPU_STATUS");
}

/* contexthub_reset_prepare: the secure firmware opens its registers to the CHUB. */
static int tzpc_set(struct chub *c)
{
	struct arm_smccc_res res;

	arm_smccc_smc(SMC_CMD_CONN_IF, (EXYNOS_SHUB << 32) | EXYNOS_SET_CONN_TZPC, 0, 0, 0, 0, 0, 0, &res);
	dev_info(c->dev, "TZPC: call to the secure firmware, result %d\n", (int)res.a0);
	return (int)res.a0 ? -EIO : 0;
}

/* ---- Shared queues: sending (ipc_write_data and ipc_add_evt of chub_ipc.c) ---- */

/* Adds an event for the CHUB and raises its interrupt. With tx_lock held. */
static int ipc_event_add(struct chub *c, u32 evt)
{
	int tries, i;

	for (tries = 0; tries < 100; tries++) {
		u32 eq = m_r32(c, EVT(IPC_EVT_A2C, ctrl.eq)), dq = m_r32(c, EVT(IPC_EVT_A2C, ctrl.dq));

		if (eq >= IPC_EVT_NUM || dq >= IPC_EVT_NUM) {
			note_anomaly(c, "event queue to the CHUB with indexes %u %u", eq, dq);
			return -EIO;
		}
		/* A free interrupt is needed both for the firmware (pending) and in the mailbox. */
		for (i = 0; (eq + 1) % IPC_EVT_NUM != dq && i < IRQ_NUM_EVT_END; i++) {
			if (readb(c->ipc + EVT(IPC_EVT_A2C, ctrl.pending) + i) ||
			    (readl(c->mbox + MBOX_INTSR1) & BIT(i)))
				continue;
			writeb(1, c->ipc + EVT(IPC_EVT_A2C, ctrl.pending) + i);
			m_w32(c, EVT_ENTRY(IPC_EVT_A2C, eq) + offsetof(struct ipc_event, irq), i);
			m_w32(c, EVT_ENTRY(IPC_EVT_A2C, eq) + offsetof(struct ipc_event, evt), evt);
			m_w32(c, EVT_ENTRY(IPC_EVT_A2C, eq) + offsetof(struct ipc_event, status), IPC_EVT_EQ);
			m_w32(c, EVT(IPC_EVT_A2C, ctrl.eq), (eq + 1) % IPC_EVT_NUM);
			writel(BIT(i), c->mbox + MBOX_INTGR1);
			return 0;
		}
		usleep_range(500, 1000);
	}
	return -EBUSY;
}

static int ipc_write(struct chub *c, const u8 *pkt, unsigned int n)
{
	u32 eq, dq;
	int tries, ret;

	if (!c->cpu_released || c->queue_broken || c->exiting)
		return -EAGAIN;
	if (n > PACKET_SIZE_MAX)
		return -EINVAL;

	mutex_lock(&c->tx_lock);
	for (tries = 0; ; tries++) {
		eq = m_r32(c, DATA(IPC_DATA_A2C, eq));
		dq = m_r32(c, DATA(IPC_DATA_A2C, dq));
		if (eq >= IPC_CH_BUF_NUM || dq >= IPC_CH_BUF_NUM) {
			note_anomaly(c, "packet queue to the CHUB with indexes %u %u", eq, dq);
			ret = -EIO;
			goto out;
		}
		if ((eq + 1) % IPC_CH_BUF_NUM != dq)
			break;
		if (tries >= 50) {
			note_anomaly(c, "packet queue to the CHUB full: the firmware is not reading");
			ret = -EBUSY;
			goto out;
		}
		usleep_range(1000, 2000);
	}
	/* The firmware reads the packet when the event arrives: first the content, then the index. */
	memcpy_toio(c->ipc + CHANNEL(IPC_DATA_A2C, eq) + offsetof(struct ipc_channel, buf), pkt, n);
	m_w32(c, CHANNEL(IPC_DATA_A2C, eq), n);
	m_w32(c, DATA(IPC_DATA_A2C, eq), (eq + 1) % IPC_CH_BUF_NUM);
	ret = ipc_event_add(c, IRQ_EVT_CH0);
	if (ret) {
		/* Like Samsung: the packet stays in the queue marked as invalid. */
		m_w32(c, CHANNEL(IPC_DATA_A2C, eq), CHANNEL_INVALID);
		note_anomaly(c, "event not queued (%d): packet cancelled", ret);
	} else {
		c->sent++;
	}
out:
	mutex_unlock(&c->tx_lock);
	return ret;
}

/* ---- Sensor protocol: sending ---- */

static int shub_send(struct chub *c, u8 cmd, u8 type, u8 subcmd, const void *data, unsigned int n)
{
	u8 pkt[SHUB_CMD_SIZE] = {};
	struct shub_header t = {
		.cmd = cmd, .type = type, .subcmd = subcmd, .length = cpu_to_le16(n),
		.timestamp = cpu_to_le64(ktime_get_boottime_ns()),
	};
	int ret;

	if (!c->alive)
		return -EAGAIN;
	if (n > PAYLOAD_MAX)
		return -EINVAL;
	memcpy(pkt, &t, sizeof(t));
	if (n)
		memcpy(pkt + sizeof(t), data, n);
	ret = ipc_write(c, pkt, sizeof(pkt));
	note(c, "tx: command %u, sensor %u, subcommand %u, %u bytes: %*ph (%d)", cmd, type, subcmd, n,
	     (int)min(n, 16u), pkt + sizeof(t), ret);
	return ret;
}

/* Sends a request and waits for the reply; returns how many bytes it copied into buf. */
static int shub_ask(struct chub *c, u8 type, u8 subcmd, u8 *buf, unsigned int max)
{
	int ret;

	mutex_lock(&c->ask_lock);
	mutex_lock(&c->reply_lock);
	c->reply.type = type;
	c->reply.subcmd = subcmd;
	c->reply.buf = buf;
	c->reply.max = max;
	c->reply.len = 0;
	c->reply.arrived = false;
	c->reply.waiting = true;
	mutex_unlock(&c->reply_lock);

	ret = shub_send(c, CMD_GETVALUE, type, subcmd, NULL, 0);
	if (!ret)
		wait_event_timeout(c->waitq, c->reply.arrived || c->exiting, msecs_to_jiffies(REPLY_MS));

	mutex_lock(&c->reply_lock);
	c->reply.waiting = false;
	if (!ret)
		ret = c->reply.arrived ? (int)c->reply.len : -ETIMEDOUT;
	mutex_unlock(&c->reply_lock);
	mutex_unlock(&c->ask_lock);
	if (ret < 0)
		note(c, "request %u/%u without reply (%d)", type, subcmd, ret);
	return ret;
}

/* ---- Sensor protocol: receiving ---- */

/* From a floating point number to billionths, without using floating point in the kernel. */
static u64 float_to_nano(u32 bits)
{
	int exponent = (bits >> 23) & 0xff;
	u64 v = (u64)((bits & 0x7fffff) | 0x800000) * 1000000000ULL;

	if (!exponent || exponent == 0xff)
		return 0;
	exponent -= 127 + 23;
	if (exponent >= 0)
		return exponent > 9 ? U64_MAX : v << exponent;
	return exponent <= -64 ? 0 : v >> -exponent;
}

/* A block of sensor data (parse_dataframe of Samsung): several entries one after the other. */
static void data_block(struct chub *c, const u8 *d, unsigned int n)
{
	unsigned int i = 0, len;
	struct sensor *s;
	u16 count;
	u8 type, entry = 0;

	while (i < n) {
		entry = d[i++];

		switch (entry) {
		case SHUB2AP_BYPASS_DATA:
			if (i + 3 > n)
				goto truncated;
			type = d[i++];
			count = get_unaligned_le16(d + i);
			i += 2;
			if (!count)
				break;
			if (type == SENSOR_TYPE_ACCELEROMETER) {
				s = &c->sensors[ACCELEROMETER];
			} else if (type == SENSOR_TYPE_GEOMAGNETIC_FIELD) {
				s = &c->sensors[COMPASS];
			} else {
				note(c, "samples of sensor %u: format not known here, rest skipped: %*ph", type,
				     (int)min(n - i, 24u), d + i);
				return;
			}
			/*
			 * Per sample, then the 64-bit firmware time: the accelerometer gives 16-bit x, y,
			 * z; the compass 32-bit x, y, z and an accuracy byte.
			 */
			len = s->type == SENSOR_TYPE_ACCELEROMETER ? 6 : 13;
			do {
				if (i + len + 8 > n)
					goto truncated;
				spin_lock(&c->sample_lock);
				if (s->type == SENSOR_TYPE_ACCELEROMETER) {
					s->x = (s16)get_unaligned_le16(d + i);
					s->y = (s16)get_unaligned_le16(d + i + 2);
					s->z = (s16)get_unaligned_le16(d + i + 4);
				} else {
					s->x = get_unaligned_le32(d + i);
					s->y = get_unaligned_le32(d + i + 4);
					s->z = get_unaligned_le32(d + i + 8);
					s->accuracy = d[i + 12];
				}
				s->time = get_unaligned_le64(d + i + len);
				s->samples++;
				spin_unlock(&c->sample_lock);
				i += len + 8;
				count--;
			} while (count > 0 && i < n);
			break;
		case SHUB2AP_DEBUG_DATA:
			if (i >= n)
				goto truncated;
			len = d[i++];
			if (!len || len > n - i)
				goto truncated;
			note(c, "[M] %.*s", (int)len, d + i);
			i += len;
			break;
		case SHUB2AP_META_DATA:
			if (i + 2 > n)
				goto truncated;
			note(c, "meta: event %u of sensor %u", d[i], d[i + 1]);
			i += 2;
			break;
		case SHUB2AP_LIBRARY_DATA:
			if (i + 2 > n)
				goto truncated;
			len = get_unaligned_le16(d + i);
			i += 2;
			if (len > n - i)
				goto truncated;
			note(c, "library data, %u bytes: %*ph", len, (int)min(len, 24u), d + i);
			i += len;
			break;
		case SHUB2AP_NOTI_RESET:
			note(c, "the firmware reports it has restarted");
			break;
		case SHUB2AP_REQ_RESET:
			if (i + 2 > n)
				goto truncated;
			note(c, "the firmware asks for a restart: reason %#x, sensor without events %u", d[i],
			     d[i + 1]);
			i += 2;
			break;
		default:
			note(c, "entry %#x not interpreted, rest skipped: %*ph", entry, (int)min(n - i, 24u),
			     d + i);
			return;
		}
	}
	return;
truncated:
	note_anomaly(c, "data block truncated at position %u of %u (entry %#x)", i, n, entry);
}

/* A packet that arrived from the firmware (handle_packet of Samsung). */
static void packet_rx(struct chub *c, const u8 *p, unsigned int n)
{
	struct shub_header t;
	const u8 *data = NULL;
	unsigned int tot, len;
	bool is_sample;

	if (n < sizeof(t)) {
		note_anomaly(c, "packet of %u bytes, shorter than the header: %*ph", n, (int)n, p);
		return;
	}
	memcpy(&t, p, sizeof(t));
	tot = le16_to_cpu(t.total_length);
	len = le16_to_cpu(t.length);
	if (len > n - sizeof(t)) {
		note_anomaly(c, "packet of %u bytes that declares %u: %*ph", n, len, (int)min(n, 32u), p);
		return;
	}
	p += sizeof(t);

	/* Samples arrive continuously: in the log the first ten, then one every fifty. */
	is_sample = t.cmd == CMD_REPORT && len && p[0] == SHUB2AP_BYPASS_DATA;
	if (!is_sample || c->samples_logged++ < 10 || c->samples_logged % 50 == 0)
		note(c, "rx: command %u, sensor %u, subcommand %u, %u bytes of %u: %*ph", t.cmd, t.type,
		     t.subcmd, len, tot, (int)min(len, 24u), p);

	if (tot && tot != len) {
		if (!c->big_total || c->big_time != le64_to_cpu(t.timestamp)) {
			c->big_time = le64_to_cpu(t.timestamp);
			c->big_total = tot;
			c->big_len = 0;
		}
		if (tot != c->big_total || tot > BIG_SIZE || c->big_len + len > tot) {
			note_anomaly(c, "inconsistent multi-packet reply (%u + %u of %u)", c->big_len,
				     len, tot);
			c->big_total = 0;
			return;
		}
		memcpy(c->big + c->big_len, p, len);
		c->big_len += len;
		if (c->big_len < tot)
			return;
		c->big_total = 0;
		data = c->big;
		len = tot;
	} else if (tot) {
		data = p;
	} else {
		len = 0;
	}

	switch (t.cmd) {
	case CMD_GETVALUE:
		mutex_lock(&c->reply_lock);
		if (c->reply.waiting && !c->reply.arrived && c->reply.type == t.type &&
		    c->reply.subcmd == t.subcmd) {
			if (len > c->reply.max)
				note(c, "reply %u/%u of %u bytes: keeping the first %u", t.type, t.subcmd, len,
				     c->reply.max);
			c->reply.len = min(len, c->reply.max);
			if (c->reply.len)
				memcpy(c->reply.buf, data, c->reply.len);
			c->reply.arrived = true;
			wake_up_all(&c->waitq);
		} else {
			note(c, "reply %u/%u that nobody was waiting for", t.type, t.subcmd);
		}
		mutex_unlock(&c->reply_lock);
		break;
	case CMD_REPORT:
		if (len)
			data_block(c, data, len);
		break;
	default:
		note(c, "command %u not expected incoming", t.cmd);
	}
}

/* ---- Shared queues: receiving ---- */

/* ipc_read_data: takes a packet off the queue from the CHUB. Returns the length, 0 if empty. */
static unsigned int ipc_read(struct chub *c)
{
	u32 eq = m_r32(c, DATA(IPC_DATA_C2A, eq)), dq = m_r32(c, DATA(IPC_DATA_C2A, dq)), n = 0;

	if (eq >= IPC_CH_BUF_NUM || dq >= IPC_CH_BUF_NUM) {
		note(c, "packet queue from the CHUB with invalid indexes (%u, %u): not reading any more", eq, dq);
		c->queue_broken = true;
		return 0;
	}
	while (dq != eq && !n) {
		u32 len = m_r32(c, CHANNEL(IPC_DATA_C2A, dq));

		if (len && len <= PACKET_SIZE_MAX) {
			memcpy_fromio(c->rx, c->ipc + CHANNEL(IPC_DATA_C2A, dq) + offsetof(struct ipc_channel, buf),
				      len);
			n = len;
		} else {
			note_anomaly(c, "packet from the CHUB of length %#x: skipped", len);
		}
		dq = (dq + 1) % IPC_CH_BUF_NUM;
	}
	m_w32(c, DATA(IPC_DATA_C2A, dq), dq);
	return n;
}

static void chub_event(struct chub *c, u32 evt)
{
	unsigned int n;
	u32 v;

	if (evt < IRQ_EVT_CH_MAX) {
		n = ipc_read(c);
		if (n) {
			c->received++;
			packet_rx(c, c->rx, n);
		}
		return;
	}
	switch (evt) {
	case IRQ_EVT_C2A_DEBUG:
		v = m_r32(c, M(dbg.event));
		m_w32(c, M(dbg.event), 0);
		note(c, "the firmware reports an error: diagnostic event %u (37 = fault, 38 = assert)", v);
		dev_err(c->dev, "the CHUB firmware reports an error (diagnostic event %u)\n", v);
		break;
	case IRQ_EVT_C2A_ASSERT:
		note(c, "the firmware reports an assert");
		break;
	case IRQ_EVT_C2A_INT:
	case IRQ_EVT_C2A_INTCLR:
		if (c->other_events++ < 8)
			note(c, "event %u from the CHUB (interrupt line of the old protocol)", evt);
		break;
	default:
		note_anomaly(c, "event %u from the CHUB not expected", evt);
	}
}

/*
 * What Samsung's interrupt handler (contexthub_irq_handler) would do, called at intervals
 * instead: "alive" signal, then the queued events, each with its own interrupt.
 */
static void events_receive(struct chub *c)
{
	u32 state = readl(c->mbox + MBOX_INTSR0), eq, dq, evt, irq, full;
	int rounds;

	if (state & BIT(IRQ_CHUB_ALIVE + IRQ_DAL_CHUB)) {
		writel(BIT(IRQ_CHUB_ALIVE + IRQ_DAL_CHUB), c->mbox + MBOX_INTCR0);
		if (readl(c->mbox + MBOX_ISSR(SR_CHUB_ALIVE)) == CHUB_REBOOT_REQ) {
			note(c, "the firmware asks to be restarted");
		} else {
			c->alive_seen = true;
			wake_up_all(&c->waitq);
		}
	}
	/* Request to read the log: it is read at every round anyway. */
	if (state & BIT(IRQ_CHUB_LOG + IRQ_DAL_CHUB))
		writel(BIT(IRQ_CHUB_LOG + IRQ_DAL_CHUB), c->mbox + MBOX_INTCR0);
	state &= GENMASK(IRQ_NUM_EVT_END - 1 + IRQ_DAL_CHUB, IRQ_DAL_CHUB);

	for (rounds = 0; state && !c->queue_broken && rounds < IPC_EVT_NUM; rounds++) {
		eq = m_r32(c, EVT(IPC_EVT_C2A, ctrl.eq));
		dq = m_r32(c, EVT(IPC_EVT_C2A, ctrl.dq));
		if (eq >= IPC_EVT_NUM || dq >= IPC_EVT_NUM) {
			note(c, "event queue from the CHUB with invalid indexes (%u, %u): not reading any more", eq, dq);
			c->queue_broken = true;
			return;
		}
		if (eq == dq) {
			/* Interrupts without an event: Samsung clears them all. */
			note_anomaly(c, "interrupts %#x from the CHUB with no queued events", state);
			writel(state, c->mbox + MBOX_INTCR0);
			return;
		}
		evt = m_r32(c, EVT_ENTRY(IPC_EVT_C2A, dq) + offsetof(struct ipc_event, evt));
		irq = m_r32(c, EVT_ENTRY(IPC_EVT_C2A, dq) + offsetof(struct ipc_event, irq));
		full = m_r32(c, EVT_ENTRY(IPC_EVT_C2A, dq) + offsetof(struct ipc_event, status));
		m_w32(c, EVT_ENTRY(IPC_EVT_C2A, dq) + offsetof(struct ipc_event, status), IPC_EVT_DQ);
		m_w32(c, EVT(IPC_EVT_C2A, ctrl.dq), (dq + 1) % IPC_EVT_NUM);
		if (full != IPC_EVT_EQ) {
			note_anomaly(c, "empty event in the queue (status %u): skipped", full);
			continue;
		}
		if (irq >= IRQ_NUM_EVT_END) {
			note_anomaly(c, "event %u with interrupt %u out of range", evt, irq);
			continue;
		}
		if (!(state & BIT(irq + IRQ_DAL_CHUB)))
			note_anomaly(c, "event %u without its interrupt %u (state %#x)", evt, irq, state);
		writel(BIT(irq + IRQ_DAL_CHUB), c->mbox + MBOX_INTCR0);
		state &= ~BIT(irq + IRQ_DAL_CHUB);
		/* Frees the interrupt: the firmware reuses it only when it sees this zero. */
		writeb(0, c->ipc + EVT(IPC_EVT_C2A, ctrl.pending) + irq);
		chub_event(c, evt);
	}
}

/* The two firmware logs: the line one (ipc_logbuf_outprint) and the character one. */
static void fw_log_read(struct chub *c)
{
	u32 eq = m_r32(c, M(log.eq)), dq = m_r32(c, M(log.dq)), size;
	struct ipc_log_line line;
	char text[LOGBUF_DATA_SIZE + 40];
	int rounds, n;

	if (eq < LOGBUF_NUM && dq < LOGBUF_NUM) {
		/* Full: the oldest lines have already been overwritten. */
		if (readb(c->ipc + M(log.full))) {
			writeb(0, c->ipc + M(log.full));
			dq = (eq + 1) % LOGBUF_NUM;
		}
		for (rounds = 0; dq != eq && rounds < LOGBUF_NUM; rounds++) {
			memcpy_fromio(&line, c->ipc + LOG_LINE(dq), sizeof(line));
			n = strnlen(line.buf, sizeof(line.buf));
			while (n && (line.buf[n - 1] == '\n' || line.buf[n - 1] == '\r'))
				n--;
			n = scnprintf(text, sizeof(text), "fw %u%s: %.*s\n", line.size, line.error ? " ERROR" : "",
				      n, line.buf);
			log_append(c, text, n);
			dq = (dq + 1) % LOGBUF_NUM;
		}
		m_w32(c, M(log.dq), dq);
		if (readb(c->ipc + M(log.flush_req)))
			writeb(0, c->ipc + M(log.flush_req));
	}

	eq = m_r32(c, M(log.raw.eq));
	dq = m_r32(c, M(log.raw.dq));
	size = m_r32(c, M(log.raw.size));
	if (!size || size > c->ipc_bytes - sizeof(struct ipc_map) || eq >= size || dq >= size || eq == dq)
		return;
	while (dq != eq) {
		n = min_t(u32, (eq > dq ? eq : size) - dq, sizeof(text));
		memcpy_fromio(text, c->ipc + M(log.raw.buf) + dq, n);
		log_append(c, text, n);
		dq = (dq + n) % size;
	}
	m_w32(c, M(log.raw.dq), dq);
}

static void receive(struct chub *c)
{
	mutex_lock(&c->rx_lock);
	events_receive(c);
	if (!c->queue_broken)
		fw_log_read(c);
	mutex_unlock(&c->rx_lock);
}

static irqreturn_t mailbox_irq(int irq, void *data)
{
	struct chub *c = data;

	c->interrupts++;
	receive(c);
	/* With the queue broken nobody clears the interrupts any more: it would stay on forever. */
	if (c->queue_broken)
		disable_irq_nosync(irq);
	return IRQ_HANDLED;
}

/* Requests the mailbox interrupt from the CPU controller. Without it, polling at intervals stays. */
static void mailbox_irq_get(struct chub *c)
{
	struct of_phandle_args spec = { .args_count = 3, .args = { GIC_SPI, SPI_MAILBOX, IRQ_TYPE_LEVEL_HIGH } };
	int irq, ret;

	spec.np = of_find_compatible_node(NULL, NULL, "arm,gic-400");
	if (!spec.np) {
		dev_warn(c->dev, "interrupt controller not found: polling the firmware at intervals\n");
		return;
	}
	irq = irq_create_of_mapping(&spec);
	of_node_put(spec.np);
	/* The sixteen interrupts from the CHUB (bits 16-31) unmasked. */
	writel(readl(c->mbox + MBOX_INTMR0) & ~(0xffffU << IRQ_DAL_CHUB), c->mbox + MBOX_INTMR0);
	ret = irq ? request_threaded_irq(irq, NULL, mailbox_irq, IRQF_ONESHOT, "chub", c) : -ENXIO;
	if (ret) {
		dev_warn(c->dev, "mailbox interrupt not available (%d): polling the firmware at intervals\n", ret);
		return;
	}
	c->irq = irq;
}

static void chub_poll(struct work_struct *work)
{
	struct chub *c = container_of(work, struct chub, poll.work);

	receive(c);
	/* Like Samsung's timer: the firmware aligns its clock to the time in the header. */
	if (c->ready && time_after(jiffies, c->next_time_sync)) {
		c->next_time_sync = jiffies + secs_to_jiffies(TIME_SYNC_SECONDS);
		shub_send(c, CMD_SETVALUE, TYPE_MCU, SUB_RTC_TIME, NULL, 0);
	}
	if (!c->exiting)
		schedule_delayed_work(&c->poll, msecs_to_jiffies(c->irq ? POLL_SLOW_MS : POLL_MS));
}

/* ---- Sensors ---- */

/* Turns on (or changes the period of) or off a sensor: enable_sensor and disable_sensor. */
static int sensor_set(struct chub *c, struct sensor *s, bool on, u32 ms)
{
	__le32 data[2] = {};
	int ret = 0;

	if (!(c->found[0] & BIT(s->type)))
		return -ENODEV;
	mutex_lock(&c->sensors_lock);
	if (on) {
		/* Sampling period and maximum delivery delay, in milliseconds. */
		ms = clamp(ms, 5u, 1000u);
		data[0] = cpu_to_le32(ms);
		ret = shub_send(c, s->on ? CMD_CHANGERATE : CMD_ADD, s->type, 0, data, sizeof(data));
		if (!ret) {
			s->ms = ms;
			s->on = true;
			c->samples_logged = 0;
		}
	} else if (s->on) {
		data[0] = cpu_to_le32(s->ms);
		ret = shub_send(c, CMD_REMOVE, s->type, 0, data, sizeof(data[0]));
		if (!ret)
			s->on = false;
	}
	mutex_unlock(&c->sensors_lock);
	return ret;
}

/* What Samsung does as soon as the firmware is alive (refresh_task): who you are, state, sensors. */
static int sensors_start(struct chub *c)
{
	struct shub_system_info info;
	struct sensor_spec spec;
	u8 pm_state = PM_COMPLETE, *list;
	__le32 axis = cpu_to_le32(ACCEL_POSITION), compass_axis = cpu_to_le32(COMPASS_POSITION);
	__le16 calibration[3] = {};
	__le32 compass_calibration[4] = {};	/* calibration_data_mmc5633: three offsets and the radius */
	int n, i, n_s;

	n = shub_ask(c, TYPE_MCU, SUB_HUB_SYSTEM_INFO, (u8 *)&info, sizeof(info));
	if (n != sizeof(info)) {
		dev_err(c->dev, "system information not received (%d, expected %zu bytes)\n", n, sizeof(info));
		return n < 0 ? n : -EIO;
	}
	c->fw_version = le32_to_cpu(info.fw_version);
	for (i = 0; i < 3; i++)
		c->found[i] = le64_to_cpu(info.scan[i]);
	dev_info(c->dev, "firmware: version %u, sensors found %#llx %#llx %#llx, accelerometer %s\n",
		 c->fw_version, c->found[0], c->found[1], c->found[2],
		 c->found[0] & BIT(SENSOR_TYPE_ACCELEROMETER) ? "present" : "MISSING");
	note(c, "firmware version %u, sensors found %#llx %#llx %#llx", c->fw_version, c->found[0],
	     c->found[1], c->found[2]);

	/* Processor awake, screen on, time. */
	shub_send(c, CMD_SETVALUE, TYPE_MCU, SUB_PM_STATE, &pm_state, 1);
	shub_send(c, CMD_SETVALUE, TYPE_MCU, SUB_LCD_ON, NULL, 0);
	shub_send(c, CMD_SETVALUE, TYPE_MCU, SUB_RTC_TIME, NULL, 0);
	c->next_time_sync = jiffies + secs_to_jiffies(TIME_SYNC_SECONDS);

	/* Sensor characteristics: a byte with the count, then a card per sensor. */
	list = kzalloc(BIG_SIZE, GFP_KERNEL);
	if (!list)
		return -ENOMEM;
	n = shub_ask(c, TYPE_MCU, SUB_SENSOR_SPEC, list, BIG_SIZE);
	if (n < 1 || n != 1 + list[0] * sizeof(spec))
		dev_warn(c->dev, "sensor list not received or of unexpected length (%d)\n", n);
	for (i = 0; n >= 1 && i < list[0] && 1 + (i + 1) * sizeof(spec) <= n; i++) {
		memcpy(&spec, list + 1 + i * sizeof(spec), sizeof(spec));
		note(c, "sensor %u: %.15s, vendor %u, period from %d to %u us, resolution %#x, range %#x",
		     spec.uid, spec.name, spec.vendor, (s32)le32_to_cpu(spec.min_delay),
		     le32_to_cpu(spec.max_delay), le32_to_cpu(spec.resolution), le32_to_cpu(spec.max_range));
		for (n_s = 0; n_s < N_SENSORS; n_s++) {
			if (spec.uid != c->sensors[n_s].type)
				continue;
			c->sensors[n_s].res_nano = float_to_nano(le32_to_cpu(spec.resolution));
			strscpy(c->sensors[n_s].name, spec.name, sizeof(c->sensors[n_s].name));
		}
	}
	kfree(list);

	if (c->found[0] & BIT(SENSOR_TYPE_ACCELEROMETER)) {
		/* sync_accelerometer_status: mounting position, then calibration (zero without factory file). */
		shub_send(c, CMD_SETVALUE, SENSOR_TYPE_ACCELEROMETER, SUB_SENSOR_AXIS, &axis, sizeof(axis));
		shub_send(c, CMD_SETVALUE, SENSOR_TYPE_ACCELEROMETER, SUB_CAL_DATA, calibration, sizeof(calibration));
	} else {
		dev_warn(c->dev, "the firmware did not find the accelerometer: sensor power or bus\n");
	}
	if (c->found[0] & BIT(SENSOR_TYPE_GEOMAGNETIC_FIELD)) {
		/* sync_magnetometer_status: position, correction matrix, calibration (zero: the firmware redoes it). */
		shub_send(c, CMD_SETVALUE, SENSOR_TYPE_GEOMAGNETIC_FIELD, SUB_SENSOR_AXIS, &compass_axis,
			  sizeof(compass_axis));
		shub_send(c, CMD_SETVALUE, SENSOR_TYPE_GEOMAGNETIC_FIELD, SUB_MAGNETIC_STATIC_MATRIX, compass_matrix,
			  sizeof(compass_matrix));
		shub_send(c, CMD_SETVALUE, SENSOR_TYPE_GEOMAGNETIC_FIELD, SUB_CAL_DATA, compass_calibration,
			  sizeof(compass_calibration));
	} else {
		dev_warn(c->dev, "the firmware did not find the compass\n");
	}
	for (n_s = 0; n_s < N_SENSORS; n_s++)
		dev_info(c->dev, "%s %s: declared resolution %llu billionths of %s per step\n",
			 c->sensors[n_s].label, c->sensors[n_s].name, c->sensors[n_s].res_nano, c->sensors[n_s].unit);
	return 0;
}

/* ---- debugfs ---- */

static int status_show(struct seq_file *s, void *v)
{
	static const struct { const char *name; u32 reg; } pmu[] = {
		{ "CHUB_CONFIGURATION 0x1f80", PMU_CHUB_CONFIGURATION }, { "CHUB_STATUS 0x1f84", PMU_CHUB_STATUS },
		{ "0x1f88", PMU_CHUB_1F88 }, { "CHUB_OPTION 0x1f8c", PMU_CHUB_OPTION },
		{ "CHUB_CPU_CONFIGURATION 0x2d00", PMU_CHUB_CPU_CONFIG },
		{ "CHUB_CPU_STATUS 0x2d04", PMU_CHUB_CPU_STATUS },
	};
	static const char * const queue[] = { "from the CHUB", "to the CHUB" };
	struct chub *c = s->private;
	struct sensor sample;
	char magic[17] = {};
	u32 value = 0, pending;
	int i, q;

	seq_printf(s, "phase: %s\n", c->phase);
	if (c->alive)
		seq_printf(s, "firmware: alive after %u ms, version %u (in the file %u)\n", c->alive_ms, c->fw_version,
			   c->file_version);
	else if (c->cpu_released)
		seq_printf(s, "firmware: no signal, %llu ms since the reset release\n",
			   div_u64(local_clock() - c->release_ns, NSEC_PER_MSEC));
	else
		seq_puts(s, "firmware: reset not released\n");
	seq_printf(s, "sensors found by the firmware: %#llx %#llx %#llx\n", c->found[0], c->found[1],
		   c->found[2]);
	for (i = 0; i < ARRAY_SIZE(pmu); i++) {
		regmap_read(c->pmu, pmu[i].reg, &value);
		seq_printf(s, "PMU %s = %#x\n", pmu[i].name, value);
	}
	seq_printf(s, "CHUB clock: %lu Hz measured, %u declared to the firmware; gate %s on arrival\n",
		   clk_get_rate(c->div_chub), c->hz, c->gate_was_open ? "open" : "closed");
	seq_printf(s, "sensor power: %s, %d uV\n",
		   regulator_is_enabled(c->vdd) > 0 ? "on" : "off", regulator_get_voltage(c->vdd));
	if (c->irq)
		seq_printf(s, "receive: on interrupt (%u so far)\n", c->interrupts);
	else
		seq_printf(s, "receive: polled every %u ms (interrupt not available)\n", POLL_MS);
	seq_printf(s, "mailbox: INTSR0 %#x (from the CHUB, bit 16-31), INTMSR0 %#x, INTSR1 %#x (to the CHUB)\n",
		   readl(c->mbox + MBOX_INTSR0), readl(c->mbox + MBOX_INTMSR0), readl(c->mbox + MBOX_INTSR1));
	seq_printf(s, "mailbox: shared registers %#x %#x %#x %#x\n", readl(c->mbox + MBOX_ISSR(0)),
		   readl(c->mbox + MBOX_ISSR(1)), readl(c->mbox + MBOX_ISSR(2)), readl(c->mbox + MBOX_ISSR(3)));
	seq_printf(s, "map: bootloader %#x-%#x, firmware %#x-%#x, queues %#x-%#x, RAM %#x-%#x\n",
		   c->map.bl_start, c->map.bl_end, c->map.code_start, c->map.code_end, c->map.ipc_start,
		   c->map.ipc_end, c->map.ram_start, c->map.ram_end);
	seq_printf(s, "messages: sent %u, received %u, anomalies %u%s\n", c->sent, c->received, c->anomalies,
		   c->queue_broken ? " (BROKEN QUEUES: not reading them any more)" : "");

	if (c->cpu_released)
		seq_printf(s, "CHUB processor: PC %#x, LR %#x, SP %#x\n", readl(c->gpr + DUMPGPR_PCR),
			   readl(c->gpr + DUMPGPR_GP0R + 4 * 14), readl(c->gpr + DUMPGPR_GP0R + 4 * 13));
	memcpy_fromio(magic, c->ipc + M(magic), 16);
	seq_printf(s, "shared memory: signature \"%.16s\", processor state seen by the firmware %u\n",
		   magic, readw(c->ipc + M(wake)));
	for (q = 0; q < 2; q++) {
		for (pending = 0, i = 0; i < IRQ_MAX; i++)
			pending |= readb(c->ipc + EVT(q, ctrl.pending) + i) ? BIT(i) : 0;
		seq_printf(s, "queues %s: packets written %u read %u; events written %u read %u, interrupts pending %#x\n",
			   queue[q], m_r32(c, DATA(q, eq)), m_r32(c, DATA(q, dq)), m_r32(c, EVT(q, ctrl.eq)),
			   m_r32(c, EVT(q, ctrl.dq)), pending);
	}
	seq_printf(s, "firmware log: lines written %u read %u, level %u, number %u; characters written %u read %u of %u\n",
		   m_r32(c, M(log.eq)), m_r32(c, M(log.dq)), readb(c->ipc + M(log.loglevel)),
		   m_r32(c, M(log.fw_num)), m_r32(c, M(log.raw.eq)), m_r32(c, M(log.raw.dq)),
		   m_r32(c, M(log.raw.size)));

	for (i = 0; i < N_SENSORS; i++) {
		spin_lock(&c->sample_lock);
		sample = c->sensors[i];
		spin_unlock(&c->sample_lock);
		seq_printf(s, "%s %s: %s, period %u ms, samples received %llu\n", sample.label, sample.name,
			   sample.on ? "on" : "off", sample.ms, sample.samples);
		seq_printf(s, "%s, last raw sample: x %d y %d z %d, accuracy %u, firmware time %llu ns\n",
			   sample.label, sample.x, sample.y, sample.z, sample.accuracy, sample.time);
		/* The text of the next line is read by mangos-bridge with a regular expression. */
		if (sample.res_nano && sample.res_nano != U64_MAX)
			seq_printf(s, "%s, last sample in thousandths of %s: x %lld y %lld z %lld; one step is %llu billionths\n",
				   sample.label, sample.unit, div_s64(sample.x * (s64)sample.res_nano, 1000000),
				   div_s64(sample.y * (s64)sample.res_nano, 1000000),
				   div_s64(sample.z * (s64)sample.res_nano, 1000000), sample.res_nano);
		else
			seq_printf(s, "%s: scale not known, the firmware did not declare the resolution\n", sample.label);
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(status);

static int log_show(struct seq_file *s, void *v)
{
	struct chub *c = s->private;
	size_t pos;

	mutex_lock(&c->log_lock);
	pos = c->log_written % LOG_SIZE;
	if (c->log_written >= LOG_SIZE)
		seq_write(s, c->log + pos, LOG_SIZE - pos);
	seq_write(s, c->log, pos);
	mutex_unlock(&c->log_lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(log);

/* Bytes written in hexadecimal, joined or separated by spaces. Returns how many, or an error. */
static int hex_bytes(const char *text, u8 *bytes, unsigned int max)
{
	unsigned int n = 0;
	int high, low;

	for (;;) {
		text = skip_spaces(text);
		if (!*text)
			return n;
		high = hex_to_bin(text[0]);
		low = high < 0 ? -1 : hex_to_bin(text[1]);
		if (low < 0 || n >= max)
			return -EINVAL;
		bytes[n++] = high << 4 | low;
		text += 2;
	}
}

static ssize_t command_write(struct file *file, const char __user *from, size_t n, loff_t *pos)
{
	struct chub *c = file->private_data;
	u8 bytes[SHUB_CMD_SIZE] = {};
	char line[256] = {};
	unsigned int on, ms = 100;
	int ret, cmd, type, subcmd, after = 0;

	if (n >= sizeof(line))
		return -EINVAL;
	if (copy_from_user(line, from, n))
		return -EFAULT;
	strim(line);
	if (sscanf(line, "accel %u %u", &on, &ms) >= 1) {
		ret = sensor_set(c, &c->sensors[ACCELEROMETER], on, ms);
	} else if (sscanf(line, "compass %u %u", &on, &ms) >= 1) {
		ret = sensor_set(c, &c->sensors[COMPASS], on, ms);
	} else if (sscanf(line, "cmd %i %i %i%n", &cmd, &type, &subcmd, &after) == 3 &&
		   !((cmd | type | subcmd) & ~0xff)) {
		ret = hex_bytes(line + after, bytes, PAYLOAD_MAX);
		if (ret >= 0)
			ret = shub_send(c, cmd, type, subcmd, bytes, ret);
	} else if (!strncmp(line, "msg ", 4)) {
		/* The whole packet, header included; the rest of the 64 bytes stays zero. */
		ret = hex_bytes(line + 4, bytes, sizeof(bytes));
		if (ret > 0) {
			note(c, "raw tx, %d bytes: %*ph", ret, min(ret, 32), bytes);
			ret = ipc_write(c, bytes, sizeof(bytes));
		} else if (!ret) {
			ret = -EINVAL;
		}
	} else {
		ret = -EINVAL;
	}
	return ret < 0 ? ret : n;
}

static const struct file_operations command_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = command_write,
};

/* ---- Start and stop ---- */

static void pmu_print(struct chub *c, const char *when)
{
	u32 cfg = 0, status = 0, x = 0, options = 0, cpu = 0, cpu_status = 0;

	regmap_read(c->pmu, PMU_CHUB_CONFIGURATION, &cfg);
	regmap_read(c->pmu, PMU_CHUB_STATUS, &status);
	regmap_read(c->pmu, PMU_CHUB_1F88, &x);
	regmap_read(c->pmu, PMU_CHUB_OPTION, &options);
	regmap_read(c->pmu, PMU_CHUB_CPU_CONFIG, &cpu);
	regmap_read(c->pmu, PMU_CHUB_CPU_STATUS, &cpu_status);
	dev_info(c->dev, "PMU %s: 0x1f80 %#x, 0x1f84 %#x, 0x1f88 %#x, 0x1f8c %#x, 0x2d00 %#x, 0x2d04 %#x\n", when,
		 cfg, status, x, options, cpu, cpu_status);
}

/* Checks the boot data inside the bootloader (ipc_get_chub_map). */
static int map_check(struct chub *c)
{
	struct chub_bootargs *m = &c->map;

	if (c->fw_bl->size < MAP_INFO_OFFSET + sizeof(*m))
		return dev_err_probe(c->dev, -EINVAL, "bootloader too short (%zu bytes)\n", c->fw_bl->size);
	memcpy(m, c->fw_bl->data + MAP_INFO_OFFSET, sizeof(*m));
	if (strncmp(m->magic, OS_UPDT_MAGIC, sizeof(OS_UPDT_MAGIC)) || m->ipc_version != IPC_VERSION)
		return dev_err_probe(c->dev, -EINVAL, "bootloader without the signature \"%s\" or of version %u instead of %u\n",
				     OS_UPDT_MAGIC, m->ipc_version, IPC_VERSION);
	if (m->code_start >= m->code_end || m->code_end > SRAM_SIZE || m->ipc_start >= m->ipc_end ||
	    m->ipc_end > SRAM_SIZE || c->fw_bl->size > m->code_start ||
	    c->fw_os->size > m->code_end - m->code_start)
		return dev_err_probe(c->dev, -EINVAL,
				     "inconsistent RAM map: firmware %#x-%#x (%zu bytes), queues %#x-%#x\n",
				     m->code_start, m->code_end, c->fw_os->size, m->ipc_start, m->ipc_end);
	c->ipc_bytes = m->ipc_end - m->ipc_start;
	if (c->ipc_bytes < sizeof(struct ipc_map))
		return dev_err_probe(c->dev, -EINVAL, "queue area of %u bytes, %zu needed\n", c->ipc_bytes,
				     sizeof(struct ipc_map));
	if (c->fw_os->size >= FW_VERSION_OFFSET + 4)
		c->file_version = get_unaligned_le32(c->fw_os->data + FW_VERSION_OFFSET);
	return 0;
}

static int chub_acquire(struct chub *c)
{
	struct device *dev = c->dev;
	struct device_node *cmu;
	int i, ret;

	c->pmu = exynos_get_pmu_regmap();
	if (IS_ERR(c->pmu))
		return dev_err_probe(dev, PTR_ERR(c->pmu), "PMU not available\n");

	ret = request_firmware(&c->fw_bl, "shub_nacho_bl.bin", dev);
	ret = ret ?: request_firmware(&c->fw_os, "shub_nacho_m13.bin", dev);
	if (ret)
		return dev_err_probe(dev, ret, "shub_nacho_bl.bin or shub_nacho_m13.bin missing from /lib/firmware\n");
	ret = map_check(c);
	if (ret)
		return ret;

	c->vdd = regulator_get_optional(dev, REGULATOR_NAME);
	if (IS_ERR(c->vdd)) {
		ret = PTR_ERR(c->vdd);
		c->vdd = NULL;
		return dev_err_probe(dev, ret, "supply \"%s\" not found\n", REGULATOR_NAME);
	}

	cmu = of_find_compatible_node(NULL, NULL, "samsung,exynos850-cmu-apm");
	if (!cmu)
		return dev_err_probe(dev, -ENODEV, "APM clock node missing\n");
	c->chub_gate = clock_get(cmu, CLK_GOUT_CLKCMU_CHUB_BUS);
	c->div_chub = clock_get(cmu, CLK_DOUT_CLKCMU_CHUB_BUS);
	of_node_put(cmu);
	if (IS_ERR(c->chub_gate) || IS_ERR(c->div_chub))
		return dev_err_probe(dev, -ENODEV, "CHUB clocks missing\n");
	/* These are not essential to talk to the firmware: if missing, go on. */
	cmu = of_find_compatible_node(NULL, NULL, "samsung,exynos850-cmu-cmgp");
	for (i = 0; cmu && i < ARRAY_SIZE(cmgp_gates); i++) {
		c->cmgp[i] = clock_get(cmu, cmgp_gates[i]);
		if (IS_ERR(c->cmgp[i]))
			dev_warn(dev, "CMGP clock %u missing\n", cmgp_gates[i]);
	}
	of_node_put(cmu);

	c->mbox = devm_ioremap(dev, MAILBOX_BASE, MAILBOX_SIZE);
	c->sram = devm_ioremap(dev, SRAM_BASE, SRAM_SIZE);
	c->gpr = devm_ioremap(dev, DUMPGPR_BASE, DUMPGPR_SIZE);
	c->baaw = devm_ioremap(dev, BAAW_BASE, BAAW_SIZE);
	if (!c->mbox || !c->sram || !c->gpr || !c->baaw)
		return -ENOMEM;
	c->ipc = c->sram + c->map.ipc_start;
	return 0;
}

/* Copies a file into the CHUB RAM and reads it back. */
static int sram_copy(struct chub *c, u32 offset, const struct firmware *fw, const char *name)
{
	u8 *readback = kvmalloc(fw->size, GFP_KERNEL);
	size_t i;
	int ret = 0;

	if (!readback)
		return -ENOMEM;
	memcpy_toio(c->sram + offset, fw->data, fw->size);
	memcpy_fromio(readback, c->sram + offset, fw->size);
	for (i = 0; i < fw->size && readback[i] == fw->data[i]; i++)
		;
	if (i < fw->size) {
		dev_err(c->dev, "%s: the CHUB RAM does not hold what is written to it (at %#zx: %#x instead of %#x)\n",
			name, offset + i, readback[i], fw->data[i]);
		ret = -EIO;
	} else {
		dev_info(c->dev, "%s: %zu bytes in the CHUB RAM at %#x, read back equal\n", name, fw->size, offset);
	}
	kvfree(readback);
	return ret;
}

static int chub_start(struct chub *c)
{
	struct device *dev = c->dev;
	u32 cfg = 0, status = 0, options = 0, cpu = 0, cpu_status = 0;
	int i, ret;

	/* 1. Sensor power. */
	c->phase = "sensor power";
	pmu_print(c, "on arrival");
	ret = regulator_enable(c->vdd);
	if (ret)
		return dev_err_probe(dev, ret, "sensor power not enabled\n");
	c->vdd_on = true;
	msleep(20);
	dev_info(dev, "sensor power \"%s\" on: %d uV\n", REGULATOR_NAME,
		 regulator_get_voltage(c->vdd));

	/*
	 * 2. Clock. Samsung brings the CHUB clock to 360 MHz (DLL source); here it stays as found:
	 * it is not verified that the DLL runs, and if it is stopped, switching hangs the kernel.
	 */
	c->phase = "clock";
	c->gate_was_open = __clk_is_enabled(c->chub_gate);
	ret = clk_prepare_enable(c->chub_gate);
	if (ret)
		return dev_err_probe(dev, ret, "CHUB clock not opened\n");
	for (i = 0; i < ARRAY_SIZE(cmgp_gates); i++)
		if (!IS_ERR_OR_NULL(c->cmgp[i]))
			clk_prepare_enable(c->cmgp[i]);
	c->hz = clk_get_rate(c->div_chub);
	dev_info(dev, "CHUB clock: %lu Hz, gate %s on arrival; declaring %u Hz to the firmware\n",
		 clk_get_rate(c->div_chub), c->gate_was_open ? "already open" : "closed", c->hz);

	/*
	 * 3. PMU. At phone boot the block is powered and its processor is in reset: this is the
	 * state Samsung starts from at first power-up, and there is nothing to do. In any other state
	 * it is done as in Samsung's restart (contexthub_reset): processor stopped, power removed and
	 * given back, then the TZPC.
	 */
	c->phase = "PMU";
	regmap_read(c->pmu, PMU_CHUB_CPU_CONFIG, &cpu);
	regmap_read(c->pmu, PMU_CHUB_CPU_STATUS, &cpu_status);
	if ((cpu | cpu_status) & 1) {
		dev_info(dev, "the CHUB processor appears released: stopping it\n");
		cpu_halt(c);
	}
	regmap_read(c->pmu, PMU_CHUB_CONFIGURATION, &cfg);
	regmap_read(c->pmu, PMU_CHUB_STATUS, &status);
	regmap_read(c->pmu, PMU_CHUB_OPTION, &options);
	regmap_read(c->pmu, PMU_CHUB_CPU_STATUS, &cpu_status);
	if (!(cfg & 1) || !(status & 1) || (options & 1) || (cpu_status & 1)) {
		if (status & 1) {
			ret = block_power_off(c);
			if (ret)
				return ret;
		}
		ret = block_power_on(c);
		if (ret)
			return ret;
		pmu_print(c, "after giving power back to the block");
		ret = tzpc_set(c);
		if (ret)
			return dev_err_probe(dev, ret, "TZPC rejected by the secure firmware\n");
	}
	regmap_read(c->pmu, PMU_CHUB_STATUS, &status);
	if (!(status & 1))
		return dev_err_probe(dev, -EIO, "the CHUB block does not appear powered (status %#x)\n", status);

	/* 4. Address window (contexthub_config_init) and clean mailbox. */
	c->phase = "address window";
	writel(BAAW_VAL_START, c->baaw + BAAW_START);
	writel(BAAW_VAL_END, c->baaw + BAAW_END);
	writel(BAAW_VAL_REMAP, c->baaw + BAAW_REMAP);
	writel(BAAW_RW_ACCESS_ENABLE, c->baaw + BAAW_PERMISSIONS);
	dev_info(dev, "address window written: read back %#x %#x %#x %#x\n", readl(c->baaw + BAAW_START),
		 readl(c->baaw + BAAW_END), readl(c->baaw + BAAW_REMAP), readl(c->baaw + BAAW_PERMISSIONS));
	dev_info(dev, "mailbox on arrival: INTSR0 %#x, INTSR1 %#x\n", readl(c->mbox + MBOX_INTSR0),
		 readl(c->mbox + MBOX_INTSR1));
	/* ipc_hw_clear_all_int_pend_reg: an "alive" left over from before would be mistaken for a new one. */
	writel(0xffffU << IRQ_DAL_CHUB, c->mbox + MBOX_INTCR0);

	/* 5. Firmware (contexthub_poweron): RAM cleared, bootloader, firmware. */
	c->phase = "firmware";
	memset_io(c->sram, 0, SRAM_SIZE);
	ret = sram_copy(c, 0, c->fw_bl, "bootloader");
	ret = ret ?: sram_copy(c, c->map.code_start, c->fw_os, "firmware");
	if (ret)
		return ret;

	/* 6. Boot data and shared memory (ipc_get_chub_map, contexthub_hw_reset). */
	c->phase = "boot data";
	writel(c->hz, c->sram + MAP_INFO_OFFSET + offsetof(struct chub_bootargs, chubclk));
	writew(BOOTMODE_COLD, c->sram + MAP_INFO_OFFSET + offsetof(struct chub_bootargs, bootmode));
	writew(LOG_LEVEL, c->sram + MAP_INFO_OFFSET + offsetof(struct chub_bootargs, runtimelog));
	m_w32(c, M(log.size), LOGBUF_TOTAL_SIZE);
	memcpy_toio(c->ipc + M(magic), CHUB_IPC_MAGIC, sizeof(CHUB_IPC_MAGIC));
	m_w32(c, M(log.raw.size), c->ipc_bytes - sizeof(struct ipc_map));
	m_w32(c, M(log.raw.eq), 0);
	m_w32(c, M(log.raw.dq), 0);
	writeb(LOG_LEVEL, c->ipc + M(log.loglevel));
	/* "There is a firmware to start" for the bootloader; processor registers visible. */
	writel(1, c->mbox + MBOX_ISSR(SR_BOOT_MODE));
	writel(1, c->gpr + DUMPGPR_CTRL);
	dev_info(dev, "boot data written: file version %u, queues at %#x (%u bytes, %zu used), log at level %u\n",
		 c->file_version, c->map.ipc_start, c->ipc_bytes, sizeof(struct ipc_map), LOG_LEVEL);

	/* 7. Reset release. From here the periodic work listens to the firmware. */
	c->phase = "reset released, waiting for the firmware";
	c->release_ns = local_clock();
	c->cpu_released = true;
	ret = cpu_release(c);
	pmu_print(c, "after the reset release");
	if (ret)
		return ret;
	mailbox_irq_get(c);
	schedule_delayed_work(&c->poll, msecs_to_jiffies(POLL_MS));

	/* 8. "Are you alive?": processor state in shared register 3, then interrupt 15. */
	for (i = 0; i < ALIVE_TRIES && !c->alive_seen; i++) {
		msleep(100);
		writel(AP_WAKE, c->mbox + MBOX_ISSR(SR_CHUB_ALIVE));
		writel(BIT(IRQ_CHUB_ALIVE), c->mbox + MBOX_INTGR1);
		wait_event_timeout(c->waitq, c->alive_seen, msecs_to_jiffies(200));
	}
	if (!c->alive_seen) {
		dev_warn(dev, "the firmware does not answer after %d requests: INTSR0 %#x, INTSR1 %#x, PC %#x; see status and log\n",
			 i, readl(c->mbox + MBOX_INTSR0), readl(c->mbox + MBOX_INTSR1),
			 readl(c->gpr + DUMPGPR_PCR));
		return 0;
	}
	c->alive_ms = div_u64(local_clock() - c->release_ns, NSEC_PER_MSEC);
	c->alive = true;
	c->phase = "firmware alive";
	dev_info(dev, "firmware alive after %u ms (%d requests)\n", c->alive_ms, i);
	note(c, "firmware alive after %u ms", c->alive_ms);
	/* ipc_logbuf_loglevel: the firmware may have cleared the log level at start. */
	writeb(LOG_LEVEL, c->ipc + M(log.loglevel));

	/* 9. Sensors. An error here does not unload the module: status and log remain to understand. */
	ret = sensors_start(c);
	if (ret) {
		dev_warn(dev, "the firmware is alive but does not answer requests (%d)\n", ret);
		return 0;
	}
	c->ready = true;
	c->phase = "sensors ready";
	ret = sensor_set(c, &c->sensors[ACCELEROMETER], true, ACCEL_PERIOD_MS);
	dev_info(dev, "accelerometer: switch-on every %u ms requested (%d)\n", ACCEL_PERIOD_MS, ret);
	ret = sensor_set(c, &c->sensors[COMPASS], true, COMPASS_PERIOD_MS);
	dev_info(dev, "compass: switch-on every %u ms requested (%d)\n", COMPASS_PERIOD_MS, ret);
	return 0;
}

/* Brings everything back to a safe state, from any point of the start. Does not turn clocks off. */
static void chub_stop(struct chub *c)
{
	int i;

	for (i = 0; c->ready && i < N_SENSORS; i++)
		sensor_set(c, &c->sensors[i], false, 0);
	c->ready = false;
	c->exiting = true;
	wake_up_all(&c->waitq);
	if (c->irq) {
		free_irq(c->irq, c);
		c->irq = 0;
	}
	cancel_delayed_work_sync(&c->poll);

	if (c->cpu_released) {
		cpu_halt(c);
		c->cpu_released = false;
		c->alive = false;
		pmu_print(c, "on exit");
	}
	if (c->vdd_on)
		regulator_disable(c->vdd);
	if (c->vdd)
		regulator_put(c->vdd);
	release_firmware(c->fw_bl);
	release_firmware(c->fw_os);
	/*
	 * The clocks stay on: turning off one the bootloader had left on can hang the phone.
	 */
	for (i = 0; i < ARRAY_SIZE(cmgp_gates); i++)
		clock_put(c->cmgp[i]);
	clock_put(c->chub_gate);
	clock_put(c->div_chub);
}

static int chub_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct chub *c;
	int ret;

	c = devm_kzalloc(dev, sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;
	c->dev = dev;
	c->phase = "resources";
	platform_set_drvdata(pdev, c);
	mutex_init(&c->tx_lock);
	mutex_init(&c->ask_lock);
	mutex_init(&c->reply_lock);
	mutex_init(&c->sensors_lock);
	c->sensors[ACCELEROMETER] = (struct sensor){ .type = SENSOR_TYPE_ACCELEROMETER, .label = "accelerometer",
						     .unit = "m/s²" };
	c->sensors[COMPASS] = (struct sensor){ .type = SENSOR_TYPE_GEOMAGNETIC_FIELD, .label = "compass",
					       .unit = "microtesla" };
	mutex_init(&c->log_lock);
	mutex_init(&c->rx_lock);
	spin_lock_init(&c->sample_lock);
	init_waitqueue_head(&c->waitq);
	INIT_DELAYED_WORK(&c->poll, chub_poll);

	ret = chub_acquire(c);
	ret = ret ?: chub_start(c);
	if (ret) {
		dev_err(dev, "stopped at phase \"%s\" (%d): putting the CHUB back in reset\n", c->phase, ret);
		chub_stop(c);
		return ret;
	}

	c->dir = debugfs_create_dir("chub", NULL);
	debugfs_create_file("status", 0400, c->dir, c, &status_fops);
	debugfs_create_file("log", 0400, c->dir, c, &log_fops);
	debugfs_create_file("command", 0200, c->dir, c, &command_fops);
	return 0;
}

static void chub_remove(struct platform_device *pdev)
{
	struct chub *c = platform_get_drvdata(pdev);

	debugfs_remove_recursive(c->dir);
	chub_stop(c);
}

static struct platform_driver chub_driver = {
	.probe = chub_probe,
	.remove = chub_remove,
	.driver = { .name = "chub-mangos" },
};

static struct platform_device *chub_pdev;

static int __init chub_init(void)
{
	int ret;

	ret = platform_driver_register(&chub_driver);
	if (ret)
		return ret;
	chub_pdev = platform_device_register_simple("chub-mangos", PLATFORM_DEVID_NONE, NULL, 0);
	if (IS_ERR(chub_pdev)) {
		platform_driver_unregister(&chub_driver);
		return PTR_ERR(chub_pdev);
	}
	/* The probe runs inside the registration and its result does not reach here: no bound driver means it failed. */
	if (!chub_pdev->dev.driver) {
		platform_device_unregister(chub_pdev);
		platform_driver_unregister(&chub_driver);
		return -ENODEV;
	}
	return 0;
}
module_init(chub_init);

static void __exit chub_exit(void)
{
	platform_device_unregister(chub_pdev);
	platform_driver_unregister(&chub_driver);
}
module_exit(chub_exit);

MODULE_DESCRIPTION("Start of the M13 sensor microcontroller with the Samsung firmware and reading of accelerometer and compass");
MODULE_FIRMWARE("shub_nacho_bl.bin");
MODULE_FIRMWARE("shub_nacho_m13.bin");
MODULE_LICENSE("GPL");
