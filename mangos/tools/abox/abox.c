// SPDX-License-Identifier: GPL-2.0-only
// Portions Copyright (c) Samsung Electronics Co., Ltd.
/*
 * Audio processor (ABOX 2.30) of the M13: starts it with the original Samsung firmware,
 * exchanges messages with it, records from the main microphone and plays on the speaker.
 *
 * On load, in order: memory for the firmware and for the samples; sysmmu of the audio block
 * programmed by hand (the firmware sees physical memory through the IOVA_* addresses); firmware
 * copied; ABOX clocks, pins and interrupt controller; core release. From there a periodic work
 * drains the queue of messages arriving from the firmware. If the firmware does not announce
 * its boot, loading fails.
 *
 * The audio block and its clocks are left on by the bootloader (clk_ignore_unused and
 * pd_ignore_unused in the boot arguments): the module never turns them off.
 *
 * Recording chain: microphone -> aud3004x codec (registers reached with the ACPM protocol) ->
 * I2S -> UAIF0 -> NSRC4 -> WDMA4, driven by the firmware with the PCM messages.
 *
 * Playback chain: RDMA0 -> SIFS0 -> UAIF1 -> I2S -> sma1303 amplifier (I2C, bus at 0x13860000,
 * address 0x1e) -> speaker. If that bus is not enabled in the device tree there is no speaker
 * and the rest works anyway.
 *
 * /dev/abox-mic is read, /dev/abox-speaker is written: 48 kHz, 16 bit, 2 channels.
 *
 * In /sys/kernel/debug/abox:
 *   status    phase reached, firmware version, registers, queues, sysmmu errors
 *   log       text: firmware log and received messages
 *   command   "volume N": amplifier volume (register 0x0a: half a decibel per step, higher
 *             number = quieter). Programs write it: the text must not change.
 *
 * Registers, values and order come from the Samsung M135F sources (sound/soc/samsung/abox/,
 * sound/soc/codecs/aud3004x.c, sound/soc/codecs/sma1303.c) and from drivers/iommu/exynos-iommu.c.
 *
 * Load it with modprobe abox; the calliope_*.bin files are in /lib/firmware.
 */
#include <linux/arm-smccc.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/ctype.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/firmware/samsung/exynos-acpm-protocol.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/math64.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/sched/clock.h>
#include <linux/seq_file.h>
#include <linux/sizes.h>
#include <linux/soc/samsung/exynos-pmu.h>
#include <linux/uaccess.h>
#include <linux/unaligned.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <dt-bindings/clock/exynos850.h>
#include <dt-bindings/interrupt-controller/arm-gic.h>

static const char bootargs[] = "slog=1 vss=1";

/* ABOX registers */
#define ABOX_BASE		0x14a50000
#define ABOX_SIZE		0x6000
#define ABOX_SYSPOWER_CTRL	0x0010
#define ABOX_SYSPOWER_STATUS	0x0014
#define ABOX_QCHANNEL_DISABLE	0x0038
#define ABOX_ROUTE_CTRL0	0x0040
#define ABOX_ROUTE_CTRL1	0x0044
#define ABOX_SPUS_CTRL_FC0	0x0200
#define ABOX_SPUS_CTRL1		0x0204
#define ABOX_SPUS_SBANK_RDMA0	0x0220
#define ABOX_SPUS_SIFS_CNT0	0x0290
#define ABOX_SPUM_CTRL0		0x0300
#define ABOX_SPUM_SBANK_NSRC4	0x0338
#define ABOX_UAIF0_CTRL0	0x0500
#define ABOX_UAIF0_CTRL1	0x0504
#define ABOX_UAIF0_STATUS	0x050c
#define ABOX_UAIF1_CTRL0	0x0510
#define ABOX_UAIF1_CTRL1	0x0514
#define ABOX_UAIF1_STATUS	0x051c
#define ABOX_RDMA0_CTRL		0x1000
#define ABOX_RDMA0_BUF_STR	0x1008
#define ABOX_RDMA0_BIT_CTRL0	0x1024
#define ABOX_RDMA0_STATUS	0x1030
#define ABOX_WDMA4_CTRL		0x3400
#define ABOX_WDMA4_BUF_STR	0x3408
#define ABOX_WDMA4_BIT_CTRL0	0x3424
#define ABOX_WDMA4_STATUS	0x3430
#define ABOX_PC_CORE0		0x507c
#define ABOX_PC_CORE1		0x50fc

#define UAIF_MODE_MASTER	BIT(2)
#define UAIF_DATA_MODE		BIT(4)
#define UAIF_IRQ_MODE		BIT(3)
#define UAIF_MIC_ENABLE		BIT(1)
#define UAIF_SPK_ENABLE		BIT(0)
/* I2S, 16 bit, 2 channels, BCLK not inverted. */
#define UAIF_CTRL1_I2S_16_2	0x0984f000
/* The same two bits for WDMA4 and RDMA0. */
#define DMA_ENABLE		BIT(0)
#define DMA_RUNNING		BIT(31)

/* Internal memory of the audio core: it sees it at address 0. */
#define SRAM_BASE		0x14b00000
#define SRAM_SIZE		0x2d000
#define SRAM_BOOTARGS		0x23e00
#define BOOTARGS_SIZE		512

#define SYSREG_BASE		0x14a10000
#define SYSREG_ARAM_CTRL	0x40c
#define TIMER_BASE		0x14ae0000
#define TIMER1_VALUE		0x630		/* 64 bit, 26 MHz */
/* Four timers, one every 0x20: command, mode, start value, current value. */
#define TIMER_N			4
#define TIMER_CTRL0(n)		(0x600 + 0x20 * (n))
#define TIMER_LOWER_IRQ		BIT(2)
#define TIMER_CTRL1(n)		(0x604 + 0x20 * (n))
#define TIMER_PRESET(n)		(0x608 + 0x20 * (n))
#define TIMER_VALUE(n)		(0x610 + 0x20 * (n))

/*
 * Pins of the audio block, function 2: gpb0-0 is the codec clock, gpb0-1..4 the I2S to the
 * codec, gpb1-0..3 the I2S to the amplifier (gpb1-4 belongs to the radio and is not touched).
 */
#define PINS_BASE		0x14a60000
#define GPB0_CON		0x0
#define GPB0_AUDIO		0x22222
#define GPB0_MASK		0xfffff
#define GPB1_CON		0x20
#define GPB1_AUDIO		0x2222
#define GPB1_MASK		0xffff

/* Speaker amplifier (sma1303): I2C bus and address, registers used here. */
#define AMP_BUS			0x13860000
#define AMP_ADDRESS		0x1e
#define AMP_SYSTEM		0x00	/* bit 0 on, bit 1 reset */
#define AMP_RESET		BIT(1)
#define AMP_RESET_MS		10	/* before then what is written may be lost */
#define AMP_VOLUME		0x0a
#define AMP_MUTE		0x0e	/* bit 0 */
#define AMP_OUTPUT		0x10	/* bit 4:2: 0 off, 1 mono */
#define AMP_PLL			0x8e	/* bit 2:0: 0 running, 7 off */
#define AMP_STATUS1		0xfa	/* all right: 0xc0 (bits 7 and 6: temperature within limits) */
#define AMP_STATUS2		0xfb	/* bits 5 and 4: too much current; bit 0: clock missing */
#define AMP_ID			0xff	/* bit 7:3 = 2: it is an sma1303 */
#define AMP_VOLUME_MAX		0x31	/* Samsung's: it does not go louder than this */
#define AMP_VOLUME_MIN		0xa7

/* CPU PMU: power of the two audio cores. */
#define PMU_PAD			0x20a0
#define PMU_PAD_NORMAL		BIT(11)
#define PMU_CORE0_POWER		0x2d80
#define PMU_CORE0_ENABLE	0x2da0
#define PMU_CORE0_STANDBY	0x2da4
#define PMU_CORE1_POWER		0x2e00
#define PMU_CORE1_ENABLE	0x2e20
#define PMU_CORE1_STANDBY	0x2e24
/* Bits saying the core is stopped (samsung,pmu_standby of the Samsung device tree). */
#define PMU_CORE0_STOPPED	0x11
#define PMU_CORE1_STOPPED	0x01

/*
 * Sysmmu of the audio block, version 7 with the "VM" register layout. Reading a register that
 * does not exist hangs the kernel: only those listed here are touched.
 */
#define SYSMMU_BASE		0x14850000
#define SYSMMU_SIZE		0x9000
#define MMU_CTRL		0x000
#define MMU_CFG			0x004
#define MMU_INT_STATUS		0x060
#define MMU_INT_CLEAR		0x064
#define MMU_VERSION		0x034
#define MMU_CAPA0		0x870
#define MMU_CAPA1		0x874
#define MMU_FAULT_VA		0x1000
#define MMU_FAULT_INFO		0x1004
#define MMU_CTRL_VM		0x8000
#define MMU_PT_BASE		0x800c
#define MMU_FLUSH_ALL		0x8010
#define MMU_BLOCK		0x7
#define MMU_ENABLE		0x5
/* Priority 15, first-level entry cache, automatic clock, permission checks. */
#define MMU_CFG_VALUE		((15 << 7) | BIT(20) | BIT(24) | BIT(2))
#define MMU_VM_ENABLE		(BIT(0) | BIT(3))	/* bit 3: on errors it stops and waits */
/* Table entries: physical address >> 4, read and write permissions, type. */
#define ENTRY_SECTION(phys)	((u32)((phys) >> 4) | (3 << 4) | 2)	/* 1 MiB */
#define ENTRY_TABLE(phys)	((u32)((phys) >> 4) | 1)
#define ENTRY_PAGE(phys)	((u32)((phys) >> 4) | (3 << 2) | 2)	/* 4 KiB */
#define TAB1_BYTES		SZ_16K		/* 4096 entries, one per MiB */
#define TAB2_ENTRIES		256		/* one per 4 KiB page */
/* The second-level tables (4 KiB) and, right after, the catch-all page. */
#define TAB2_BYTES		SZ_8K

/* Addresses seen by the firmware. */
#define IOVA_DRAM		0x80000000
#define IOVA_PCM		0x92400000	/* what Samsung gives WDMA4 */
#define IOVA_SOUND		0x91000000	/* what Samsung gives RDMA0 */
#define IOVA_CHIPID		0x10000000	/* page mapped onto itself */
#define DRAM_BYTES		0x1400000
#define PCM_BYTES		SZ_1M
#define DRAM_QUEUE_TX		0x850000	/* from us to the firmware */
#define DRAM_QUEUE_RX		0x868000	/* from the firmware to us */
#define QUEUE_BYTES		0x18000

/* Private interrupt controller of the ABOX: reachable only through the secure firmware. */
#define GICD			0x14af1000
#define GICC			0x14af2000
#define GICD_CTLR		0x000
#define GICD_TYPER		0x004	/* bit 4:0: groups of 32 interrupts, minus one */
#define GICD_ISENABLER		0x100
#define GICD_ICPENDR		0x280
#define GICD_ICACTIVER		0x380
#define GICD_ITARGETSR		0x800
#define GICD_SGIR		0xf00
#define GICC_CTLR		0x00
#define GICC_PMR		0x04
#define GICC_IAR		0x0c
#define GICC_EOIR		0x10
#define GICC_DIR		0x1000
#define SGI_ABOX_MSG		15
#define IRQ_WDT			106
#define TARGET_CORE0		0
#define TARGET_AP		2
#define SMC_CMD_REG		((unsigned long)-101)
#define SMC_REG_ID_SFR_W(a)	((0x1UL << 30) | ((a) >> 2))
#define SMC_REG_ID_SFR_R(a)	((0x3UL << 30) | ((a) >> 2))

/* Messages: categories and types used here (include/sound/samsung/abox_ipc.h of Samsung). */
#define IPC_SYSTEM		1
#define IPC_PCMPLAYBACK		2
#define IPC_PCMCAPTURE		3
#define IPC_ABOX_CONFIG		0xa
#define ABOX_RESUME		2
#define ABOX_BOOT_DONE		3
#define ABOX_CHANGE_GEAR	4
#define ABOX_CHANGED_GEAR	9
#define ABOX_REPORT_LOG		0x10
#define ABOX_FLUSH_LOG		0x11
#define ABOX_REPORT_FAULT	0xfa
#define ABOX_CPU_GEAR_BOOT	0xb00d
#define SET_SIFS0_FORMAT	0x10
#define SET_SIFM4_FORMAT	0x18
#define PCM_PLTDAI_OPEN		11
#define PCM_PLTDAI_CLOSE	12
#define PCM_PLTDAI_HW_PARAMS	14
#define PCM_PLTDAI_HW_FREE	15
#define PCM_PLTDAI_PREPARE	16
#define PCM_PLTDAI_TRIGGER	17
#define PCM_PLTDAI_POINTER	18
#define PCM_SET_BUFFER		20
#define PCM_PLTDAI_ACK		22
#define PCM_PLTDAI_CLOSED	23
#define WDMA_ID			4
#define RDMA_ID			0

/* Codec: pages of the power chip on ACPM channel 2. */
#define PMIC_CHANNEL		2
#define D			0x07	/* digital part */
#define A			0x08	/* analog part */
/* Microphone analog gain: 0 = 0 dB, 1 = +12 dB, 2 = +20 dB. */
#define MIC_GAIN		2

#define AUDIF_HZ		73728000
#define MCLK_HZ			24576000
#define AUDIF_DIVIDER		14
#define BCLK_DIVIDER		48		/* 1.536 MHz = 48000 x 2 channels x 16 bit */
#define BYTES_PER_PERIOD	1024		/* 256 samples, 5.3 ms */
/*
 * 256 periods: the buffer is 65536 samples long, as much as the WDMA4 counter (see
 * capture_position). The firmware accepts at most 786432 bytes and stops on a rejected buffer.
 */
#define PERIODS			256
#define RING_BYTES		(BYTES_PER_PERIOD * PERIODS)
/* How far ahead of RDMA0 the writer of the samples to play is when it (re)starts: 11 ms. */
#define LEAD_BYTES		(2 * BYTES_PER_PERIOD)

#define LOG_SIZE		SZ_64K
#define POLL_MS			5
/* With the interrupt the periodic round only serves the firmware log, which does not notify. */
#define POLL_SLOW_MS		500
/* The interrupt of the ABOX controller towards us (abox_gic node of Samsung). */
#define SPI_ABOX		64
#define IRQ_IDLE_MAX		100
#define BOOT_MS			3000

/* A queue command: the summary of the message and where its data block is. */
struct abox_command {
	s32 id, cmd, arg[3], data_idx;
	u64 send, recv, time_put, time_get;
};

/* A message queue, shared with the firmware. */
struct abox_queue {
	s32 cmd_s, cmd_e;		/* read (the receiver), write (the sender) */
	struct abox_command cmd[128];
	s32 data_s, data_e, data_len;
	u8 data[];			/* blocks: s32 size, then the message */
};

#define QUEUE_COMMANDS		128
#define DATA_LEN		(QUEUE_BYTES - (int)offsetof(struct abox_queue, data_s) - 12)

/*
 * A message (ABOX_IPC_MSG of Samsung, 768 bytes). p[0] is the type. For system and
 * configuration messages p[1..3] are the parameters; for PCM ones p[1] is the channel and the
 * parameters start at p[2]. In system messages the two 64-bit values are in p[4..5] and p[6..7].
 */
struct abox_message {
	s32 ipcid, task_id;
	s32 p[190];
};

static_assert(sizeof(struct abox_command) == 56);
static_assert(offsetof(struct abox_queue, data) == 0x1c14);
static_assert(sizeof(struct abox_message) == 768);

/* Header of a firmware text log (ABOX_LOG_BUFFER). */
struct abox_fw_log {
	u32 write_pos, read_pos, size;
	char text[];
};

static const struct {
	const char *name;
	bool in_dram;
	u32 offset;
} binaries[] = {
	{ "calliope_sram.bin", false, 0x0 },
	{ "calliope_dram.bin", true, 0x0 },
	{ "calliope_sram_2.bin", false, 0x11000 },
	{ "calliope_dram_2.bin", true, 0x280000 },
};

/* Gates of the audio block clocks that must stay open. */
static const unsigned int audio_gates[] = {
	CLK_GOUT_AUD_ABOX_ACLK, CLK_GOUT_AUD_CA32_CCLK, CLK_GOUT_AUD_ASB_CCLK,
	CLK_GOUT_AUD_SYSMMU_CLK, CLK_GOUT_AUD_SYSREG_PCLK, CLK_GOUT_AUD_CNT_BCLK,
	CLK_GOUT_AUD_CODEC_MCLK, CLK_GOUT_AUD_UAIF0_BCLK, CLK_GOUT_AUD_UAIF1_BCLK,
};

/* Amplifier registers: { register, mask, value }. */
struct amp_step {
	u8 reg, mask, value;
};

/* sma1303_reset of Samsung, after the reset and the volume. The amplifier stays off and muted. */
static const struct amp_step amp_init[] = {
	{ 0x0b, 0xc0, 0x80 }, { 0x0c, 0x08, 0x00 }, { AMP_MUTE, 0xff, 0x3f }, { AMP_OUTPUT, 0xff, 0x04 },
	{ 0x11, 0x01, 0x01 },	/* a single amplifier: the two channels summed */
	{ 0x14, 0xff, 0x5c }, { 0x3b, 0xff, 0x5a }, { 0x3c, 0x20, 0x20 }, { 0x3f, 0x0c, 0x0c },
	{ 0x90, 0xff, 0x26 }, { 0x91, 0xff, 0x42 }, { 0x92, 0xff, 0xe0 }, { 0x94, 0xff, 0x35 },
	{ 0x37, 0xff, 0x27 }, { 0x95, 0xff, 0x0c }, { 0x97, 0xff, 0x95 }, { 0x96, 0xff, 0x42 },
	{ 0xa2, 0x80, 0x80 },
	/* PLL locked to the bit clock (1.536 MHz -> 24.576 MHz), 16-bit I2S, not master. */
	{ 0xa2, 0x48, 0x08 }, { 0x8b, 0xff, 0x07 }, { 0x8c, 0xff, 0xe0 }, { 0x8d, 0xff, 0x8b },
	{ 0x8f, 0xff, 0x03 }, { 0xa2, 0x04, 0x00 }, { 0x01, 0x78, 0x00 },
};

static const struct amp_step amp_on[] = {
	{ AMP_PLL, 0x07, 0x00 }, { AMP_SYSTEM, 0x01, 0x01 }, { AMP_OUTPUT, 0x1c, 0x04 },
	{ AMP_MUTE, 0x01, 0x00 },
};

/* After the mute Samsung waits 55 ms before the rest: whoever uses the table does it. */
static const struct amp_step amp_off[] = {
	{ AMP_OUTPUT, 0x1c, 0x00 }, { AMP_SYSTEM, 0x01, 0x00 }, { AMP_PLL, 0x07, 0x07 },
};

/* Bits of abox.in_use: the microphone and the speaker are opened one at a time. */
enum { USE_MIC, USE_SPEAKER };

struct abox {
	struct device *dev;
	struct acpm_handle *acpm;
	struct regmap *pmu;
	void __iomem *sfr, *sram, *sysreg, *timer, *pins, *sysmmu;
	struct clk *audif, *bclk, *bclk1, *select1, *mclk, *cpu, *gate[ARRAY_SIZE(audio_gates)];
	const struct firmware *fw[ARRAY_SIZE(binaries)];

	/* Memory: DMA addresses are physical addresses (no translation on our side). */
	void *dram, *pcm, *sound;
	u32 *tab1, *tab2;	/* tab2: four 1 KiB tables (empty, identity, samples, sound) */
	dma_addr_t dram_phys, pcm_phys, sound_phys, tab1_phys, tab2_phys;
	u32 mmu_ctrl, mmu_vm;	/* values found, to be put back on exit */
	bool sysmmu_enabled, gic_ready, core_released;

	const char *phase;
	struct abox_queue *tx, *rx;
	struct mutex tx_lock;
	struct delayed_work poll;
	struct mutex listen_lock;	/* interrupt and periodic round do not listen together */
	int irq;			/* 0: not available, polled at intervals */
	unsigned int interrupts, idle_irqs;
	wait_queue_head_t waitq;
	bool boot_done, exiting, queue_broken, gic_acks, irq_disabled;
	unsigned long in_use;		/* one bit per open device: USE_MIC, USE_SPEAKER */
	spinlock_t update_lock;		/* changes to single register bits are not lost */
	u8 amp_copy[256];		/* the amplifier registers as we wrote them */
	DECLARE_BITMAP(amp_known, 256);
	u32 version, boot_ms;
	char version_text[5];
	u64 release_ns;
	unsigned int received, sent, other_irqs;
	struct { u32 id, address; } log[4];
	unsigned int n_log;

	/* Capture */
	unsigned int pointers;
	u32 last_pointer;
	bool capturing, pcm_closed, ack_requested;
	bool opened, started;		/* how far the capture start got */
	size_t read_off;		/* how far whoever reads /dev/abox-mic has read, in the buffer */
	struct miscdevice mic;
	bool mic_registered;

	/* Playback */
	struct i2c_adapter *amp_bus;
	struct i2c_client *amp;		/* NULL: there is no speaker */
	struct miscdevice speaker;
	bool speaker_registered;
	bool sound_opened, sound_started, sound_closed, sound_ack;
	unsigned int sound_pointers, sound_underruns;
	u32 sound_last;
	/*
	 * Inside the ring, under sound_lock: how far RDMA0 has already read and been cleared to
	 * zero (cleaned), and how far new samples have been written (written).
	 */
	struct mutex sound_lock;
	size_t cleaned, written;
	bool to_play;			/* samples written that RDMA0 has not finished reading yet */

	struct dentry *dir;
	struct mutex log_lock;
	u64 log_written;
	char log_buf[LOG_SIZE];
};

/* ---- Text log ---- */

static void log_append(struct abox *a, const char *text, size_t n)
{
	mutex_lock(&a->log_lock);
	for (; n; n--)
		a->log_buf[a->log_written++ % LOG_SIZE] = *text++;
	mutex_unlock(&a->log_lock);
}

static __printf(2, 3) void note(struct abox *a, const char *fmt, ...)
{
	char line[200];
	va_list args;
	u32 rem;
	u64 s = div_u64_rem(local_clock(), NSEC_PER_SEC, &rem);
	int n = scnprintf(line, sizeof(line) - 1, "[%5llu.%03u] ", s, rem / 1000000);

	va_start(args, fmt);
	n += vscnprintf(line + n, sizeof(line) - 1 - n, fmt, args);
	va_end(args);
	line[n++] = '\n';
	log_append(a, line, n);
}

/* ---- Simple accessors ---- */

static void abox_update(struct abox *a, u32 reg, u32 mask, u32 value)
{
	spin_lock(&a->update_lock);
	writel((readl(a->sfr + reg) & ~mask) | value, a->sfr + reg);
	spin_unlock(&a->update_lock);
}

static int codec(struct abox *a, u8 page, u8 reg, u8 mask, u8 value)
{
	int ret = a->acpm->ops->pmic.update_reg(a->acpm, PMIC_CHANNEL, page, reg, 0, value, mask);

	if (ret)
		dev_err(a->dev, "codec: page %#x register %#x not written (%d)\n", page, reg, ret);
	return ret;
}

/*
 * The amplifier bus makes mistakes, especially while the amplifier is powering up: an
 * unacknowledged exchange that works when repeated, a confirmed write that does not stick, a
 * read that returns a wrong value without an error. Modifying a register starting from what is
 * read at that moment sometimes leaves the speaker muted. For this reason a copy of the
 * registers is kept: the value to write is computed from it, every write is read back, and a
 * wrong read can only make the write repeat.
 */

/* A register read until two reads in a row agree. */
static int amp_read(struct abox *a, u8 reg)
{
	int prev = -EIO, cur, round;

	for (round = 0; round < 6; round++) {
		cur = i2c_smbus_read_byte_data(a->amp, reg);
		if (cur >= 0 && cur == prev)
			return cur;
		prev = cur;
		if (cur < 0)
			usleep_range(2000, 3000);
	}
	note(a, "amplifier: register %#x not readable (%d)", reg, cur);
	return cur < 0 ? cur : -EIO;
}

static int amp_update(struct abox *a, u8 reg, u8 mask, u8 value)
{
	bool reset = reg == AMP_SYSTEM && (value & AMP_RESET);
	int ret, round, last = -EIO, new_value;

	if (mask != 0xff && !test_bit(reg, a->amp_known)) {
		ret = amp_read(a, reg);
		if (ret < 0)
			return ret;
		a->amp_copy[reg] = ret;
		set_bit(reg, a->amp_known);
	}
	new_value = (a->amp_copy[reg] & ~mask) | value;
	for (round = 0; round < 5; round++) {
		ret = i2c_smbus_write_byte_data(a->amp, reg, new_value);
		if (ret >= 0 && reset) {
			/* All registers go back to factory values, and the bit goes back to zero by itself. */
			bitmap_zero(a->amp_known, 256);
			return 0;
		}
		if (ret >= 0)
			ret = i2c_smbus_read_byte_data(a->amp, reg);
		/* The written bits are there, and the others are the same as a moment ago: it is the
		 * register that has bits of its own that change, not the write that went wrong. */
		if (ret == new_value || (ret >= 0 && ret == last && (ret & mask) == value)) {
			a->amp_copy[reg] = ret;
			set_bit(reg, a->amp_known);
			return 0;
		}
		last = ret;
		note(a, "amplifier: register %#x, written %#x and read back %d, attempt %d", reg, new_value, ret,
		     round + 1);
		usleep_range(2000, 3000);
	}
	dev_err(a->dev, "amplifier: register %#x not written (%d)\n", reg, last);
	return last < 0 ? last : -EIO;
}

static int amp_run(struct abox *a, const struct amp_step *steps, unsigned int n)
{
	int ret = 0;

	for (; n && !ret; n--, steps++)
		ret = amp_update(a, steps->reg, steps->mask, steps->value);
	return ret;
}

static struct clk *clock_get(struct device_node *cmu, unsigned int id)
{
	struct of_phandle_args spec = { .np = cmu, .args_count = 1, .args = { id } };

	return of_clk_get_from_provider(&spec);
}

/* Reports a clock more than 1% away from the wanted frequency: the audio would come out at another speed. */
static void clock_check(struct abox *a, const char *name, struct clk *clk, unsigned long wanted)
{
	unsigned long hz = clk_get_rate(clk);

	if (hz < wanted - wanted / 100 || hz > wanted + wanted / 100)
		dev_warn(a->dev, "clock %s at %lu Hz instead of %lu\n", name, hz, wanted);
}

static int gic_read(u32 address, u32 *value)
{
	struct arm_smccc_res res;

	arm_smccc_smc(SMC_CMD_REG, SMC_REG_ID_SFR_R(address), 0, 0, 0, 0, 0, 0, &res);
	if (res.a0)
		return -EIO;
	*value = res.a2;
	return 0;
}

static int gic_write(u32 address, u32 value)
{
	struct arm_smccc_res res;

	arm_smccc_smc(SMC_CMD_REG, SMC_REG_ID_SFR_W(address), value, 0, 0, 0, 0, 0, &res);
	return res.a0 ? -EIO : 0;
}

/* ---- Sysmmu ---- */

static int sysmmu_enable(struct abox *a)
{
	u32 version = readl(a->sysmmu + MMU_VERSION);
	u32 capa0 = readl(a->sysmmu + MMU_CAPA0), capa1 = readl(a->sysmmu + MMU_CAPA1);
	u32 *identity = a->tab2 + TAB2_ENTRIES, *samples = a->tab2 + 2 * TAB2_ENTRIES;
	u32 *sound = a->tab2 + 3 * TAB2_ENTRIES;
	unsigned int i;

	a->mmu_ctrl = readl(a->sysmmu + MMU_CTRL);
	a->mmu_vm = readl(a->sysmmu + MMU_CTRL_VM);
	dev_info(a->dev, "sysmmu: version %#x, capabilities %#x %#x, control %#x, VM %#x\n", version,
		 capa0, capa1, a->mmu_ctrl, a->mmu_vm);
	if (((version >> 21) & 0x7ff) >> 7 != 7 || !(capa0 & BIT(11)) || !(capa1 & BIT(14))) {
		dev_err(a->dev, "sysmmu: not version 7 with VM registers\n");
		return -ENODEV;
	}
	if ((a->mmu_ctrl & BIT(0)) || (a->mmu_vm & BIT(0))) {
		dev_err(a->dev, "sysmmu: already on, not touching it\n");
		return -EBUSY;
	}

	/*
	 * Everything that is not mapped points to a single catch-all page. The firmware also writes
	 * to areas not needed here (the memory shared with the modem at 0xA0000000, its register at
	 * 0xE0000000): an access without a mapping stops the sysmmu and the firmware with it, and
	 * it stays stuck even after the core is turned off, until the phone is rebooted.
	 */
	for (i = 0; i < TAB2_ENTRIES; i++)
		a->tab2[i] = ENTRY_PAGE(a->tab2_phys + SZ_4K);
	for (i = 0; i < TAB1_BYTES / sizeof(u32); i++)
		a->tab1[i] = ENTRY_TABLE(a->tab2_phys);
	for (i = 0; i < DRAM_BYTES / SZ_1M; i++)
		a->tab1[(IOVA_DRAM >> 20) + i] = ENTRY_SECTION(a->dram_phys + i * SZ_1M);
	a->tab1[IOVA_CHIPID >> 20] = ENTRY_TABLE(a->tab2_phys + SZ_1K);
	a->tab1[IOVA_PCM >> 20] = ENTRY_TABLE(a->tab2_phys + 2 * SZ_1K);
	a->tab1[IOVA_SOUND >> 20] = ENTRY_TABLE(a->tab2_phys + 3 * SZ_1K);
	for (i = 0; i < TAB2_ENTRIES; i++) {
		identity[i] = ENTRY_PAGE(a->tab2_phys + SZ_4K);
		samples[i] = ENTRY_PAGE(a->pcm_phys + i * SZ_4K);
		sound[i] = ENTRY_PAGE(a->sound_phys + i * SZ_4K);
	}
	identity[0] = ENTRY_PAGE((u64)IOVA_CHIPID);
	wmb();

	writel(MMU_BLOCK, a->sysmmu + MMU_CTRL);
	/* An error left marked by a previous boot would be mistaken for a new one. */
	writel(readl(a->sysmmu + MMU_INT_STATUS), a->sysmmu + MMU_INT_CLEAR);
	writel(MMU_CFG_VALUE, a->sysmmu + MMU_CFG);
	writel(a->tab1_phys >> 12, a->sysmmu + MMU_PT_BASE);
	writel(1, a->sysmmu + MMU_FLUSH_ALL);
	writel(a->mmu_vm | MMU_VM_ENABLE, a->sysmmu + MMU_CTRL_VM);
	writel(MMU_ENABLE, a->sysmmu + MMU_CTRL);
	a->sysmmu_enabled = true;
	return 0;
}

static void sysmmu_disable(struct abox *a)
{
	writel(a->mmu_ctrl, a->sysmmu + MMU_CTRL);
	writel(0, a->sysmmu + MMU_CFG);
	writel(a->mmu_vm, a->sysmmu + MMU_CTRL_VM);
	a->sysmmu_enabled = false;
}

/*
 * Error state of the sysmmu: says at which address the firmware tried to access without a
 * mapping. After an error the sysmmu stays stopped and the firmware with it.
 */
static void sysmmu_fault(struct abox *a, char *text, size_t n)
{
	u32 state, info;

	if (!a->sysmmu_enabled) {
		scnprintf(text, n, "off");
		return;
	}
	state = readl(a->sysmmu + MMU_INT_STATUS);
	if (!state) {
		scnprintf(text, n, "no error");
		return;
	}
	info = readl(a->sysmmu + MMU_FAULT_INFO);
	scnprintf(text, n, "ERROR: state %#x (type %u), %s at address %#x, information %#x", state,
		  (unsigned int)__ffs(state), info & BIT(20) ? "write" : "read",
		  readl(a->sysmmu + MMU_FAULT_VA), info);
}

/* ---- Messages ---- */

static int ipc_send(struct abox *a, const struct abox_message *m, bool wait)
{
	const int n = 4 + sizeof(*m);
	struct abox_command c = { .id = m->ipcid, .cmd = m->p[0], .time_put = local_clock() };
	struct abox_queue *q = a->tx;
	s32 s, e, ce, room, size = sizeof(*m);
	int tries, ret = 0;

	if (!a->boot_done)
		return -EAGAIN;
	switch (m->ipcid) {
	case IPC_SYSTEM:
		c.arg[2] = m->p[3];
		fallthrough;
	case IPC_ABOX_CONFIG:
		c.arg[1] = m->p[2];
		fallthrough;
	default:
		c.arg[0] = m->p[1];
	}

	mutex_lock(&a->tx_lock);
	for (tries = 0; ; tries++) {
		s = READ_ONCE(q->data_s);
		e = q->data_e;
		ce = q->cmd_e;
		if (s < 0 || s >= DATA_LEN || e < 0 || e >= DATA_LEN || ce < 0 || ce >= QUEUE_COMMANDS) {
			note(a, "queue to the firmware with invalid indexes: %d %d %d", s, e, ce);
			ret = -EIO;
			goto out;
		}
		/* A block is not split across the end: if it does not fit, start again from zero. */
		room = s > e ? s - e : DATA_LEN - e;
		if (s <= e && room <= n) {
			e = 0;
			room = s;
		}
		if (room > n && READ_ONCE(q->cmd_s) != (ce + 1) % QUEUE_COMMANDS)
			break;
		if (tries >= 20) {
			note(a, "queue to the firmware full: message %d/%d not sent", m->ipcid, m->p[0]);
			ret = -EIO;
			goto out;
		}
		usleep_range(1000, 2000);
	}
	memcpy(q->data + e, &size, 4);
	memcpy(q->data + e + 4, m, sizeof(*m));
	q->data_e = (e + n) % DATA_LEN;
	c.data_idx = e;
	q->cmd[ce] = c;
	wmb();
	WRITE_ONCE(q->cmd_e, (ce + 1) % QUEUE_COMMANDS);
	a->sent++;

	/* Software interrupt 15 to ABOX core 0: "there is a message". */
	if (gic_write(GICD + GICD_SGIR, BIT(16) | SGI_ABOX_MSG)) {
		note(a, "signal to the firmware rejected by the secure firmware (message %d/%d)", m->ipcid,
		     m->p[0]);
		ret = -EIO;
	} else if (wait && read_poll_timeout(READ_ONCE, s, s == q->cmd_e, 500, 100000, false,
					      q->cmd_s)) {
		note(a, "message %d/%d not read by the firmware within 100 ms", m->ipcid, m->p[0]);
		ret = -ETIMEDOUT;
	}
out:
	mutex_unlock(&a->tx_lock);
	return ret;
}

/* A PCM message: dir is IPC_PCMCAPTURE (WDMA4) or IPC_PCMPLAYBACK (RDMA0). */
static int pcm_send(struct abox *a, int dir, int type, int p0, int p1, int p2, bool wait)
{
	int channel = dir == IPC_PCMPLAYBACK ? RDMA_ID : WDMA_ID;
	struct abox_message m = { .ipcid = dir, .task_id = channel, .p = { type, channel, p0, p1, p2 } };

	return ipc_send(a, &m, wait);
}

static void boot_finished(struct abox *a)
{
	struct abox_message m = { .ipcid = IPC_SYSTEM, .p = { ABOX_RESUME } };
	u64 now = local_clock(), abox_ns;
	char *v = a->version_text;
	int i;

	if (a->boot_done)
		return;
	a->boot_ms = div_u64(now - a->release_ns, NSEC_PER_MSEC);
	a->phase = "firmware started";
	a->boot_done = true;
	for (i = 0; i < 4; i++)
		v[i] = isprint((a->version >> (24 - 8 * i)) & 0xff) ? a->version >> (24 - 8 * i) : '?';
	dev_info(a->dev, "firmware started in %u ms, version %s (%#x)\n", a->boot_ms, v, a->version);
	note(a, "boot done after %u ms, version %s (%#x)", a->boot_ms, v, a->version);

	/* Resume: the kernel time and that of the ABOX timer (26 MHz), in nanoseconds. */
	abox_ns = div_u64((readl(a->timer + TIMER1_VALUE) |
			  (u64)readl(a->timer + TIMER1_VALUE + 4) << 32) * 500, 13);
	m.p[4] = lower_32_bits(now);
	m.p[5] = upper_32_bits(now);
	m.p[6] = lower_32_bits(abox_ns);
	m.p[7] = upper_32_bits(abox_ns);
	if (ipc_send(a, &m, false))
		dev_err(a->dev, "resume message not sent\n");
	wake_up_all(&a->waitq);
}

static void dispatch(struct abox *a, const struct abox_message *m)
{
	const s32 *p = m->p;

	if (m->ipcid == IPC_SYSTEM) {
		switch (p[0]) {
		case ABOX_BOOT_DONE:
			a->version = p[3];
			boot_finished(a);
			return;
		case ABOX_CHANGE_GEAR: {
			/* The firmware asks for a frequency: it is answered with the one it really has. */
			u32 ns;
			u64 s = div_u64_rem(local_clock(), NSEC_PER_SEC, &ns);
			struct abox_message r = {
				.ipcid = IPC_SYSTEM,
				.p = { ABOX_CHANGED_GEAR, clk_get_rate(a->cpu), s, ns },
			};

			note(a, "CHANGE_GEAR level %d, requester %#x: answering %d Hz", p[1], p[2], r.p[1]);
			/* With this requester it also counts as "boot done". */
			if (p[2] == ABOX_CPU_GEAR_BOOT)
				boot_finished(a);
			ipc_send(a, &r, false);
			return;
		}
		case ABOX_REPORT_LOG:
			note(a, "REPORT_LOG number %d at address %#x", p[1], p[2]);
			if ((u32)p[2] < IOVA_DRAM)
				note(a, "firmware log outside its DRAM: not read");
			else if (a->n_log < ARRAY_SIZE(a->log))
				a->log[a->n_log++] = (typeof(a->log[0])){ p[1], p[2] };
			else
				note(a, "too many firmware logs: not reading this one");
			return;
		case ABOX_FLUSH_LOG:
			return;
		case ABOX_REPORT_FAULT:
			dev_err(a->dev, "the firmware reports a fault: type %d, %#x %#x\n", p[1], p[2], p[3]);
			break;
		}
		note(a, "system: type %#x, parameters %#x %#x %#x", p[0], p[1], p[2], p[3]);
		return;
	}
	if (m->ipcid == IPC_PCMCAPTURE) {
		switch (p[0]) {
		case PCM_PLTDAI_POINTER:
			a->last_pointer = p[2];
			if (++a->pointers <= 5)
				note(a, "PCM_PLTDAI_POINTER %u: channel %d, position %#x", a->pointers, p[1],
				     p[2]);
			/* If the firmware asked for them, it is told how far we have read. */
			if (a->ack_requested)
				pcm_send(a, IPC_PCMCAPTURE, PCM_PLTDAI_ACK, (u32)p[2] - IOVA_PCM, 0, 0, false);
			return;
		case PCM_PLTDAI_ACK:
			a->ack_requested = p[2];
			break;
		case PCM_PLTDAI_CLOSED:
			a->pcm_closed = true;
			wake_up_all(&a->waitq);
			break;
		}
		note(a, "capture: type %d, channel %d, parameters %#x %#x", p[0], p[1], p[2], p[3]);
		return;
	}
	if (m->ipcid == IPC_PCMPLAYBACK) {
		switch (p[0]) {
		case PCM_PLTDAI_POINTER:
			a->sound_last = p[2];
			if (++a->sound_pointers <= 5)
				note(a, "playback, PCM_PLTDAI_POINTER %u: channel %d, position %#x, RDMA0 %#x",
				     a->sound_pointers, p[1], p[2], readl(a->sfr + ABOX_RDMA0_STATUS));
			return;
		case PCM_PLTDAI_ACK:
			a->sound_ack = p[2];
			break;
		case PCM_PLTDAI_CLOSED:
			a->sound_closed = true;
			wake_up_all(&a->waitq);
			break;
		}
		note(a, "playback: type %d, channel %d, parameters %#x %#x", p[0], p[1], p[2], p[3]);
		return;
	}
	note(a, "category %d: type %#x, parameters %#x %#x %#x", m->ipcid, p[0], p[1], p[2], p[3]);
}

static void ipc_receive(struct abox *a)
{
	struct abox_queue *q = a->rx;
	struct abox_message m;
	struct abox_command c;
	int rounds;

	for (rounds = 0; rounds < QUEUE_COMMANDS && !a->queue_broken; rounds++) {
		s32 s = READ_ONCE(q->cmd_s), e = READ_ONCE(q->cmd_e), size = 0;

		if (s == e)
			return;
		/* The firmware writes the content and then the index: once the index is read, the content must be read after. */
		dma_rmb();
		if (s < 0 || s >= QUEUE_COMMANDS || e < 0 || e >= QUEUE_COMMANDS) {
			note(a, "queue from the firmware with invalid indexes (%d, %d): not reading it any more", s, e);
			a->queue_broken = true;
			return;
		}
		c = q->cmd[s];
		memset(&m, 0, sizeof(m));
		if (c.data_idx >= 0 && c.data_idx <= DATA_LEN - 4)
			memcpy(&size, q->data + c.data_idx, 4);
		if (size > 0 && size <= DATA_LEN - 4 - c.data_idx) {
			memcpy(&m, q->data + c.data_idx + 4, min_t(size_t, size, sizeof(m)));
			q->data_s = (c.data_idx + 4 + size) % DATA_LEN;
		} else {
			/* Without a valid block, the summary in the command remains. */
			note(a, "invalid data block (position %d, size %d)", c.data_idx,
			     size);
			m.ipcid = c.id;
			m.p[0] = c.cmd;
			memcpy(&m.p[1], c.arg, sizeof(c.arg));
		}
		q->cmd[s].time_get = local_clock();
		WRITE_ONCE(q->cmd_s, (s + 1) % QUEUE_COMMANDS);
		a->received++;
		dispatch(a, &m);
	}
}

/* Copies into the log the new text of the logs the firmware has declared. */
static void fw_log_read(struct abox *a)
{
	unsigned int i;

	for (i = 0; i < a->n_log; i++) {
		u32 offset = a->log[i].address - IOVA_DRAM, write_pos, read_pos, size;
		struct abox_fw_log *l;

		if (offset > DRAM_BYTES - sizeof(*l))
			continue;
		l = a->dram + offset;
		write_pos = READ_ONCE(l->write_pos);
		dma_rmb();
		read_pos = l->read_pos;
		size = l->size;
		if (!size || size > DRAM_BYTES - sizeof(*l) - offset || write_pos >= size ||
		    read_pos >= size || read_pos == write_pos)
			continue;
		if (read_pos > write_pos) {
			log_append(a, l->text + read_pos, size - read_pos);
			read_pos = 0;
		}
		log_append(a, l->text + read_pos, write_pos - read_pos);
		l->read_pos = write_pos;
	}
}

/*
 * The interrupt towards us stays high until the ABOX controller is told what it has pending:
 * it is asked for it and closed, as Samsung's handler would do. It also serves to know whether
 * the firmware watchdog fired. Returns how many it acknowledged.
 */
static int gic_ack(struct abox *a)
{
	u32 v, id;
	int i;

	for (i = 0; a->gic_acks && i < 8; i++) {
		if (gic_read(GICC + GICC_IAR, &v)) {
			note(a, "read of the ABOX interrupts rejected: not acknowledging them any more");
			a->gic_acks = false;
			break;
		}
		id = v & 0x3ff;
		if (id >= 1020)
			break;
		if (gic_write(GICC + GICC_EOIR, v) || (id < 16 && gic_write(GICC + GICC_DIR, v)))
			a->gic_acks = false;
		if (id == IRQ_WDT)
			dev_err_ratelimited(a->dev, "firmware watchdog fired\n");
		if (id != SGI_ABOX_MSG && a->other_irqs++ < 16)
			note(a, "interrupt %u from the ABOX controller%s", id,
			     id == IRQ_WDT ? ": firmware watchdog" : "");
	}
	return i;
}

/*
 * Where RDMA0 is reading inside the ring, in bytes: its status register gives the period (8
 * bits: the ring has 256 on purpose) and the bytes already read within the period.
 */
static size_t sound_position(struct abox *a)
{
	u32 state = readl(a->sfr + ABOX_RDMA0_STATUS);

	return (((state >> 20) & 0xff) * BYTES_PER_PERIOD + (state & 0xfffff)) % RING_BYTES & ~3UL;
}

/*
 * RDMA0 reads the ring round and round without knowing how far it was written: what it has
 * already played is set back to zero, so if the writer falls behind silence is heard and not
 * the sound of one round before. If RDMA0 has reached the writer, the writer restarts a little
 * ahead of it.
 */
static void sound_clean(struct abox *a)
{
	size_t now, played, part;

	mutex_lock(&a->sound_lock);
	if (a->sound_started) {
		now = sound_position(a);
		played = (now + RING_BYTES - a->cleaned) % RING_BYTES;
		if (played > (a->written + RING_BYTES - a->cleaned) % RING_BYTES) {
			a->written = (now + LEAD_BYTES) % RING_BYTES;
			/* Counts only if there was sound: when empty it happens all the time. */
			a->sound_underruns += a->to_play;
			a->to_play = false;
		}
		part = min(played, (size_t)RING_BYTES - a->cleaned);
		memset(a->sound + a->cleaned, 0, part);
		memset(a->sound, 0, played - part);
		a->cleaned = now;
	}
	mutex_unlock(&a->sound_lock);
}

static int listen(struct abox *a)
{
	int acked;

	mutex_lock(&a->listen_lock);
	acked = gic_ack(a);
	ipc_receive(a);
	fw_log_read(a);
	sound_clean(a);
	mutex_unlock(&a->listen_lock);
	return acked;
}

/*
 * The firmware calls us for every message and, while a stream is open, at every period (5 ms):
 * it is the rhythm sound_clean needs, and at rest nothing arrives.
 */
static irqreturn_t abox_irq(int irq, void *data)
{
	struct abox *a = data;

	a->interrupts++;
	a->idle_irqs = listen(a) ? 0 : a->idle_irqs + 1;
	/*
	 * If it can no longer be acknowledged to the controller, or if it fires continuously without
	 * the controller having anything for us, the interrupt would stay on forever: go back to
	 * polling at intervals.
	 */
	if (!a->gic_acks || a->idle_irqs > IRQ_IDLE_MAX) {
		disable_irq_nosync(irq);
		a->irq_disabled = true;
	}
	return IRQ_HANDLED;
}

/* Requests the interrupt from the CPU controller. Without it, polling at intervals stays. */
static void abox_irq_get(struct abox *a)
{
	struct of_phandle_args spec = { .args_count = 3, .args = { GIC_SPI, SPI_ABOX, IRQ_TYPE_LEVEL_HIGH } };
	int irq, ret;

	spec.np = of_find_compatible_node(NULL, NULL, "arm,gic-400");
	if (!spec.np) {
		dev_warn(a->dev, "interrupt controller not found: polling the firmware at intervals\n");
		return;
	}
	irq = irq_create_of_mapping(&spec);
	of_node_put(spec.np);
	ret = irq ? request_threaded_irq(irq, NULL, abox_irq, IRQF_ONESHOT, "abox", a) : -ENXIO;
	if (ret) {
		dev_warn(a->dev, "ABOX interrupt not available (%d): polling the firmware at intervals\n", ret);
		return;
	}
	a->irq = irq;
}

static void abox_poll(struct work_struct *work)
{
	struct abox *a = container_of(work, struct abox, poll.work);
	bool irq_driven = a->irq && !a->irq_disabled;

	listen(a);
	if (!a->exiting)
		schedule_delayed_work(&a->poll, msecs_to_jiffies(irq_driven ? POLL_SLOW_MS : POLL_MS));
}

/* ---- Capture ---- */

/* Waits; returns true if exit was requested in the meantime. */
static bool wait_ms(struct abox *a, unsigned int ms)
{
	return wait_event_timeout(a->waitq, a->exiting, msecs_to_jiffies(ms));
}

static int codec_prepare(struct abox *a)
{
	int ret;

	/* Restart of the internal clock, digital power, mute, bias. */
	codec(a, D, 0xd0, 0x01, 0x00);
	codec(a, D, 0xd0, 0x01, 0x01);
	msleep(20);
	codec(a, D, 0xcc, 0x70, 0x60);
	codec(a, D, 0x30, 0x07, 0x07);
	msleep(10);
	codec(a, A, 0x17, 0x30, 0x10);
	ret = codec(a, D, 0x14, 0x01, 0x01);
	if (ret)
		return ret;
	/* Main microphone path: input, mixer, bias, microphone 1. */
	codec(a, D, 0x31, 0x77, 0x10);
	codec(a, D, 0x1e, 0x0c, 0x04);
	codec(a, D, 0xb5, 0x40, 0x40);
	codec(a, A, 0x10, 0x20, 0x20);
	codec(a, A, 0x10, 0x05, 0x05);
	codec(a, D, 0x1f, 0x01, 0x01);
	codec(a, A, 0x34, 0x60, MIC_GAIN << 5);
	codec(a, D, 0x34, 0xff, 0x56);
	codec(a, D, 0x35, 0xff, 0x56);
	/* Format: 16-bit I2S, 32 clocks per sample, 48 kHz; restart of the digital core. */
	codec(a, D, 0x21, 0x3f, 0x10);
	codec(a, D, 0x22, 0xff, 0x20);
	codec(a, D, 0x14, 0x02, 0x00);
	usleep_range(1000, 2000);
	codec(a, D, 0x14, 0x02, 0x02);
	return codec(a, D, 0x29, 0x07, 0x00);
}

static void codec_on(struct abox *a)
{
	codec(a, D, 0x10, 0x25, 0x25);
	codec(a, D, 0x12, 0x06, 0x06);
	codec(a, D, 0x37, 0x0f, 0x07);
	codec(a, D, 0x33, 0xff, 0x00);
	codec(a, D, 0x14, 0x28, 0x28);
	usleep_range(1000, 2000);
	codec(a, D, 0x18, 0x20, 0x20);
	msleep(20);
}

/* Mute, then power off the blocks in Samsung's order. */
static void codec_off(struct abox *a)
{
	codec(a, D, 0x30, 0x07, 0x07);
	msleep(10);
	codec(a, D, 0x14, 0x28, 0x00);
	codec(a, D, 0x18, 0x20, 0x00);
	codec(a, D, 0x12, 0x06, 0x00);
	codec(a, D, 0x10, 0x25, 0x00);
	codec(a, D, 0xb5, 0x40, 0x00);
	codec(a, A, 0x10, 0x25, 0x00);
	codec(a, D, 0x1f, 0x01, 0x00);
	codec(a, D, 0x1e, 0x0c, 0x00);
}

/*
 * Where WDMA4 is writing inside the buffer, in bytes. Its status register counts the samples
 * (four bytes each) on 16 bits: the buffer is 65536 samples long on purpose, so the counter is
 * the position, with no bookkeeping.
 */
static size_t capture_position(struct abox *a)
{
	return ((readl(a->sfr + ABOX_WDMA4_STATUS) >> 12) & 0xffff) * 4;
}

/*
 * Starts the capture from the main microphone: codec, interface and routing written here, the
 * rest requested from the firmware. From then on WDMA4 fills the buffer round and round. Call
 * with the microphone open (USE_MIC).
 */
static int capture_start(struct abox *a)
{
	struct abox_message format = { .ipcid = IPC_ABOX_CONFIG, .p = { SET_SIFM4_FORMAT, 16, 2 } };
	u32 state;
	int ret;

	if (!a->boot_done)
		return -EAGAIN;
	a->capturing = true;
	a->opened = a->started = false;
	a->pointers = 0;
	a->last_pointer = 0;
	a->pcm_closed = false;
	a->ack_requested = false;
	a->read_off = 0;
	ret = codec_prepare(a);
	if (ret)
		return ret;

	/* Interface to the codec and routing UAIF0 -> NSRC4 -> WDMA4. */
	abox_update(a, ABOX_QCHANNEL_DISABLE, BIT(2), BIT(2));
	writel(UAIF_CTRL1_I2S_16_2, a->sfr + ABOX_UAIF0_CTRL1);
	abox_update(a, ABOX_UAIF0_CTRL0, UAIF_MODE_MASTER | UAIF_DATA_MODE | UAIF_IRQ_MODE | UAIF_MIC_ENABLE,
		    UAIF_MODE_MASTER | UAIF_DATA_MODE);
	abox_update(a, ABOX_ROUTE_CTRL1, 0x1f << 20, 8 << 20);	/* NSRC4 takes from UAIF0 */
	abox_update(a, ABOX_SPUM_CTRL0, BIT(23) | BIT(20), 0);	/* NSRC4 -> WDMA, no conversion */
	abox_update(a, ABOX_SPUM_SBANK_NSRC4, 0x3ff << 20, 8 << 20);	/* 128-byte bank */
	abox_update(a, ABOX_WDMA4_BIT_CTRL0, 0x3, 1);		/* 16-bit output */

	/*
	 * The rest of WDMA4 is programmed by the firmware from these messages. The frequency is not
	 * sent: the M13 firmware rejects SET_SIFM4_RATE and works at the interface's one.
	 */
	ret = ipc_send(a, &format, true);
	ret = ret ?: pcm_send(a, IPC_PCMCAPTURE, PCM_PLTDAI_OPEN, 0, 0, 0, true);
	if (ret)
		return ret;
	a->opened = true;
	ret = pcm_send(a, IPC_PCMCAPTURE, PCM_SET_BUFFER, IOVA_PCM, BYTES_PER_PERIOD, PERIODS, true);
	ret = ret ?: pcm_send(a, IPC_PCMCAPTURE, PCM_PLTDAI_HW_PARAMS, 48000, 16, 2, true);
	ret = ret ?: pcm_send(a, IPC_PCMCAPTURE, PCM_PLTDAI_PREPARE, 0, 0, 0, true);
	if (ret)
		return ret;

	/* It receives: interface, then the codec blocks, then the write to memory. */
	abox_update(a, ABOX_UAIF0_CTRL0, UAIF_MIC_ENABLE, UAIF_MIC_ENABLE);
	codec_on(a);
	ret = pcm_send(a, IPC_PCMCAPTURE, PCM_PLTDAI_TRIGGER, 1, 0, 0, true);
	if (ret)
		return ret;
	/* The reader starts from where WDMA4 is writing now, not from the start of the buffer. */
	if (readl_poll_timeout(a->sfr + ABOX_WDMA4_STATUS, state, state & DMA_RUNNING, 500, 200000))
		note(a, "WDMA4 did not start within 200 ms of the start (state %#x)", state);
	a->read_off = capture_position(a) & ~3UL;
	a->started = true;
	/* The codec wants 320 ms before taking off the mute: the start of the capture is silent. */
	if (!wait_ms(a, 320))
		codec(a, D, 0x30, 0x07, 0x00);
	return 0;
}

/* Stops the capture, from whatever point the start had reached. With the microphone open (USE_MIC). */
static void capture_stop(struct abox *a, int result)
{
	u32 state;

	if (a->started)
		pcm_send(a, IPC_PCMCAPTURE, PCM_PLTDAI_TRIGGER, 0, 0, 0, true);
	if (a->opened) {
		pcm_send(a, IPC_PCMCAPTURE, PCM_PLTDAI_HW_FREE, 0, 0, 0, true);
		if (readl_poll_timeout(a->sfr + ABOX_WDMA4_STATUS, state, !(state & DMA_RUNNING), 1000,
				       1000000))
			note(a, "WDMA4 still running one second after HW_FREE (state %#x)", state);
		pcm_send(a, IPC_PCMCAPTURE, PCM_PLTDAI_CLOSE, 0, 0, 0, true);
		if (!wait_event_timeout(a->waitq, a->pcm_closed, secs_to_jiffies(1)))
			note(a, "PCM_PLTDAI_CLOSED did not arrive within one second");
	}
	a->opened = a->started = false;
	usleep_range(1000, 2000);
	abox_update(a, ABOX_UAIF0_CTRL0, UAIF_MIC_ENABLE, 0);
	codec_off(a);
	abox_update(a, ABOX_ROUTE_CTRL1, 0x1f << 20, 0);
	note(a, "capture finished (%d)", result);
	a->capturing = false;
}

/* ---- /dev/abox-mic: the microphone as a stream ---- */

/*
 * Opening the device turns the microphone on, closing it turns it off; one reader at a time.
 * Samples are read at 48 kHz, 16 bit, two channels that do not coincide.
 */
static int mic_open(struct inode *inode, struct file *file)
{
	struct abox *a = container_of(file->private_data, struct abox, mic);
	int ret;

	if (test_and_set_bit(USE_MIC, &a->in_use))
		return -EBUSY;
	ret = capture_start(a);
	if (ret) {
		capture_stop(a, ret);
		clear_bit(USE_MIC, &a->in_use);
	}
	return ret;
}

static int mic_release(struct inode *inode, struct file *file)
{
	struct abox *a = container_of(file->private_data, struct abox, mic);

	capture_stop(a, 0);
	clear_bit(USE_MIC, &a->in_use);
	return 0;
}

static ssize_t mic_read(struct file *file, char __user *buf, size_t n, loff_t *pos)
{
	struct abox *a = container_of(file->private_data, struct abox, mic);
	size_t avail, part;

	for (;;) {
		avail = (capture_position(a) + RING_BYTES - a->read_off) % RING_BYTES;
		if (avail)
			break;
		if (a->exiting || !a->started)
			return 0;
		if (msleep_interruptible(10))
			return -ERESTARTSYS;
	}
	part = min3(n & ~3UL, avail, (size_t)RING_BYTES - a->read_off);
	if (!part)
		return -EINVAL;
	if (copy_to_user(buf, a->pcm + a->read_off, part))
		return -EFAULT;
	a->read_off = (a->read_off + part) % RING_BYTES;
	return part;
}

static const struct file_operations mic_fops = {
	.owner = THIS_MODULE,
	.open = mic_open,
	.release = mic_release,
	.read = mic_read,
};

/* ---- Playback ---- */

static void amp_status_log(struct abox *a, const char *when)
{
	note(a, "amplifier %s: 0xfa = %#x, 0xfb = %#x, 0xff = %#x, system %#x", when,
	     i2c_smbus_read_byte_data(a->amp, AMP_STATUS1), i2c_smbus_read_byte_data(a->amp, AMP_STATUS2),
	     i2c_smbus_read_byte_data(a->amp, AMP_ID), i2c_smbus_read_byte_data(a->amp, AMP_SYSTEM));
}

/*
 * Starts playback on the speaker: interface and routing written here, RDMA0 requested from the
 * firmware, the amplifier last, when its clock (the bit clock) is already there. From then on
 * RDMA0 reads the ring round and round. Call with the speaker open (USE_SPEAKER).
 */
static int sound_start(struct abox *a)
{
	struct abox_message format = { .ipcid = IPC_ABOX_CONFIG, .p = { SET_SIFS0_FORMAT, 16, 2 } };
	u32 state;
	int ret;

	if (!a->boot_done)
		return -EAGAIN;
	a->sound_opened = a->sound_started = a->sound_closed = a->sound_ack = false;
	a->sound_pointers = 0;
	a->sound_last = 0;
	memset(a->sound, 0, PCM_BYTES);

	/* Interface to the amplifier and routing RDMA0 -> SIFS0 -> UAIF1. */
	abox_update(a, ABOX_QCHANNEL_DISABLE, BIT(3), BIT(3));
	writel(UAIF_CTRL1_I2S_16_2, a->sfr + ABOX_UAIF1_CTRL1);
	abox_update(a, ABOX_UAIF1_CTRL0, UAIF_MODE_MASTER | UAIF_SPK_ENABLE, UAIF_MODE_MASTER);
	abox_update(a, ABOX_SPUS_CTRL_FC0, 0x7f, 1 << 1);		/* RDMA0 -> SIFS0, no conversion */
	abox_update(a, ABOX_ROUTE_CTRL0, 0xf << 4, 1 << 4);		/* UAIF1 transmits SIFS0 */
	abox_update(a, ABOX_SPUS_CTRL1, 0x1f, 9);			/* SIFS0 at 16 bit, 2 channels */
	abox_update(a, ABOX_SPUS_SBANK_RDMA0, 0x3ff << 20, 8 << 20);	/* 128-byte bank */
	abox_update(a, ABOX_SPUS_SIFS_CNT0, 0xffffff, 0);
	abox_update(a, ABOX_RDMA0_BIT_CTRL0, 0x3, 1);			/* 16-bit input */
	abox_update(a, ABOX_RDMA0_CTRL, 0xf << 19, 4 << 19);

	ret = ipc_send(a, &format, true);
	ret = ret ?: pcm_send(a, IPC_PCMPLAYBACK, PCM_PLTDAI_OPEN, 0, 0, 0, true);
	if (ret)
		return ret;
	a->sound_opened = true;
	ret = pcm_send(a, IPC_PCMPLAYBACK, PCM_SET_BUFFER, IOVA_SOUND, BYTES_PER_PERIOD, PERIODS, true);
	ret = ret ?: pcm_send(a, IPC_PCMPLAYBACK, PCM_PLTDAI_HW_PARAMS, 48000, 16, 2, true);
	ret = ret ?: pcm_send(a, IPC_PCMPLAYBACK, PCM_PLTDAI_PREPARE, 0, 0, 0, true);
	if (ret)
		return ret;

	abox_update(a, ABOX_UAIF1_CTRL0, UAIF_SPK_ENABLE, UAIF_SPK_ENABLE);
	ret = pcm_send(a, IPC_PCMPLAYBACK, PCM_PLTDAI_TRIGGER, 1, 0, 0, true);
	if (ret)
		return ret;
	if (readl_poll_timeout(a->sfr + ABOX_RDMA0_STATUS, state, state & DMA_RUNNING, 500, 200000))
		note(a, "RDMA0 did not start within 200 ms of the start (state %#x)", state);
	ret = amp_run(a, amp_on, ARRAY_SIZE(amp_on));
	msleep(20);
	amp_status_log(a, "on");

	/* The writer starts a little ahead of where RDMA0 is reading now. */
	mutex_lock(&a->sound_lock);
	a->cleaned = sound_position(a);
	a->written = (a->cleaned + LEAD_BYTES) % RING_BYTES;
	a->sound_underruns = 0;
	a->to_play = false;
	a->sound_started = true;
	mutex_unlock(&a->sound_lock);
	return ret;
}

/* Stops playback, from whatever point the start had reached. With the speaker open. */
static void sound_stop(struct abox *a, int result)
{
	u32 state;

	/* The amplifier first, while it still has its clock: mute, wait, power off. */
	amp_status_log(a, "before turning it off");
	amp_update(a, AMP_MUTE, 0x01, 0x01);
	msleep(55);
	amp_run(a, amp_off, ARRAY_SIZE(amp_off));

	mutex_lock(&a->sound_lock);
	if (a->sound_started)
		pcm_send(a, IPC_PCMPLAYBACK, PCM_PLTDAI_TRIGGER, 0, 0, 0, true);
	a->sound_started = false;
	mutex_unlock(&a->sound_lock);
	if (a->sound_opened) {
		pcm_send(a, IPC_PCMPLAYBACK, PCM_PLTDAI_HW_FREE, 0, 0, 0, true);
		if (readl_poll_timeout(a->sfr + ABOX_RDMA0_STATUS, state, !(state & DMA_RUNNING), 1000, 1000000))
			note(a, "RDMA0 still running one second after HW_FREE (state %#x)", state);
		pcm_send(a, IPC_PCMPLAYBACK, PCM_PLTDAI_CLOSE, 0, 0, 0, true);
		if (!wait_event_timeout(a->waitq, a->sound_closed, secs_to_jiffies(1)))
			note(a, "playback: PCM_PLTDAI_CLOSED did not arrive within one second");
	}
	a->sound_opened = false;
	usleep_range(1000, 2000);
	abox_update(a, ABOX_UAIF1_CTRL0, UAIF_SPK_ENABLE, 0);
	abox_update(a, ABOX_ROUTE_CTRL0, 0xf << 4, 0);
	abox_update(a, ABOX_QCHANNEL_DISABLE, BIT(3), 0);
	note(a, "playback finished (%d), ran out of samples %u times", result, a->sound_underruns);
}

/* ---- /dev/abox-speaker: the speaker as a stream ---- */

/*
 * Opening the device turns the speaker on, closing it turns it off after playing what is left;
 * only one at a time. Samples are written at 48 kHz, 16 bit, two channels (the amplifier sums
 * them).
 */
static int speaker_open(struct inode *inode, struct file *file)
{
	struct abox *a = container_of(file->private_data, struct abox, speaker);
	int ret;

	if (test_and_set_bit(USE_SPEAKER, &a->in_use))
		return -EBUSY;
	ret = sound_start(a);
	if (ret) {
		sound_stop(a, ret);
		clear_bit(USE_SPEAKER, &a->in_use);
	}
	return ret;
}

static int speaker_release(struct inode *inode, struct file *file)
{
	struct abox *a = container_of(file->private_data, struct abox, speaker);
	unsigned int rounds;

	/* The ring lasts 1.4 seconds: when RDMA0 reaches the writer, everything has been played. */
	for (rounds = 0; rounds < 150 && a->to_play && !a->exiting; rounds++)
		msleep(10);
	sound_stop(a, 0);
	clear_bit(USE_SPEAKER, &a->in_use);
	return 0;
}

static ssize_t speaker_write(struct file *file, const char __user *buf, size_t n, loff_t *pos)
{
	struct abox *a = container_of(file->private_data, struct abox, speaker);
	size_t part;

	if (n < 4)
		return -EINVAL;
	for (;;) {
		mutex_lock(&a->sound_lock);
		/* Writes only where RDMA0 has already passed, and never up to reaching it. */
		part = min3(n & ~3UL, (a->cleaned + RING_BYTES - a->written - 4) % RING_BYTES,
			    (size_t)RING_BYTES - a->written);
		if (part)
			break;
		mutex_unlock(&a->sound_lock);
		if (a->exiting || !a->sound_started)
			return -EIO;
		if (msleep_interruptible(10))
			return -ERESTARTSYS;
	}
	if (copy_from_user(a->sound + a->written, buf, part)) {
		mutex_unlock(&a->sound_lock);
		return -EFAULT;
	}
	a->written = (a->written + part) % RING_BYTES;
	a->to_play = true;
	mutex_unlock(&a->sound_lock);
	/* If the firmware asked for them, it is told how far we have written. */
	if (a->sound_ack)
		pcm_send(a, IPC_PCMPLAYBACK, PCM_PLTDAI_ACK, a->written, 0, 0, false);
	return part;
}

static const struct file_operations speaker_fops = {
	.owner = THIS_MODULE,
	.open = speaker_open,
	.release = speaker_release,
	.write = speaker_write,
};

/*
 * Looks for the amplifier and prepares it, off and muted. Without its bus enabled in the device
 * tree, or if it does not answer like an sma1303, the speaker simply is not there.
 */
static void amp_setup(struct abox *a)
{
	struct device_node *np;
	struct resource r;
	int id, round;

	for_each_compatible_node(np, NULL, "samsung,s3c2440-i2c") {
		if (of_address_to_resource(np, 0, &r) || r.start != AMP_BUS)
			continue;
		a->amp_bus = of_get_i2c_adapter_by_node(np);
		of_node_put(np);
		break;
	}
	if (!a->amp_bus) {
		dev_warn(a->dev, "speaker: the amplifier I2C bus is not enabled in this kernel\n");
		return;
	}
	a->amp = i2c_new_dummy_device(a->amp_bus, AMP_ADDRESS);
	if (IS_ERR(a->amp)) {
		dev_warn(a->dev, "speaker: amplifier address not available (%ld)\n", PTR_ERR(a->amp));
		a->amp = NULL;
		return;
	}
	/*
	 * The first exchange after a pause of the bus can fail or read 0xff: a transient error
	 * here would take the speaker away until the next load.
	 */
	for (round = 0; round < 5; round++) {
		id = i2c_smbus_read_byte_data(a->amp, AMP_ID);
		if (id >= 0 && (id & 0xf8) == 0x10)
			break;
		note(a, "amplifier: identity not read (%d), attempt %d", id, round + 1);
		usleep_range(5000, 6000);
	}
	if (id < 0 || (id & 0xf8) != 0x10) {
		dev_warn(a->dev, "speaker: the amplifier does not answer like an sma1303 (%d)\n", id);
		goto drop;
	}
	/* Register reset, then Samsung's values with our volume. */
	if (amp_update(a, AMP_SYSTEM, AMP_RESET, AMP_RESET) < 0)
		goto drop;
	msleep(AMP_RESET_MS);
	/* It starts from the minimum volume: the bridge sets the chosen volume as soon as it sees the speaker. */
	if (amp_update(a, AMP_SYSTEM, 0xff, 0x80) < 0 ||
	    amp_update(a, AMP_VOLUME, 0xff, AMP_VOLUME_MIN) < 0 ||
	    amp_run(a, amp_init, ARRAY_SIZE(amp_init)) < 0)
		goto drop;
	dev_info(a->dev, "speaker: amplifier ready (identity %#x, status %#x %#x)\n", id,
		 i2c_smbus_read_byte_data(a->amp, AMP_STATUS1), i2c_smbus_read_byte_data(a->amp, AMP_STATUS2));
	return;
drop:
	i2c_unregister_device(a->amp);
	a->amp = NULL;
}

/* ---- debugfs ---- */

static void timer_text(struct abox *a, char *text, size_t n)
{
	size_t done = 0;
	int i;

	for (i = 0; i < TIMER_N; i++)
		done += scnprintf(text + done, n - done, "%s%d: command %#x mode %#x start %#x now %#x",
				  i ? "; " : "", i, readl(a->timer + TIMER_CTRL0(i)),
				  readl(a->timer + TIMER_CTRL1(i)), readl(a->timer + TIMER_PRESET(i)),
				  readl(a->timer + TIMER_VALUE(i)));
}

/*
 * The text of some lines ("timer N: ... now", "queue to firmware: commands read N written N") is
 * read by scripts/tests/abox_reload.py: it must not be changed without updating it.
 */
static int status_show(struct seq_file *s, void *v)
{
	static const struct { const char *name; u32 reg; } pmu[] = {
		{ "core0 power 0x2d80", PMU_CORE0_POWER }, { "core0 enable 0x2da0", PMU_CORE0_ENABLE },
		{ "core0 standby 0x2da4", PMU_CORE0_STANDBY }, { "core1 power 0x2e00", PMU_CORE1_POWER },
		{ "core1 enable 0x2e20", PMU_CORE1_ENABLE }, { "core1 standby 0x2e24", PMU_CORE1_STANDBY },
	};
	struct abox *a = s->private;
	char text[320];
	u32 value = 0;
	int i;

	seq_printf(s, "phase: %s\n", a->phase);
	if (a->boot_done)
		seq_printf(s, "firmware: started in %u ms, version %s (%#x)\n", a->boot_ms,
			   a->version_text, a->version);
	else
		seq_printf(s, "firmware: not started, %llu ms since the core release\n",
			   div_u64(local_clock() - a->release_ns, NSEC_PER_MSEC));
	for (i = 0; i < ARRAY_SIZE(pmu); i++) {
		regmap_read(a->pmu, pmu[i].reg, &value);
		seq_printf(s, "PMU %s = %#x\n", pmu[i].name, value);
	}
	seq_printf(s, "SYSPOWER_CTRL 0x10 = %#x, SYSPOWER_STATUS 0x14 = %#x\n",
		   readl(a->sfr + ABOX_SYSPOWER_CTRL), readl(a->sfr + ABOX_SYSPOWER_STATUS));
	seq_printf(s, "PC core0 = %#x, PC core1 = %#x\n", readl(a->sfr + ABOX_PC_CORE0),
		   readl(a->sfr + ABOX_PC_CORE1));
	if (!gic_read(GICD + GICD_CTLR, &value))
		seq_printf(s, "GICD_CTLR = %#x\n", value);
	timer_text(a, text, sizeof(text));
	seq_printf(s, "timer %s\n", text);
	for (i = 0; i < 4; i++) {
		u32 enabled = 0, pending = 0, active = 0;

		if (gic_read(GICD + GICD_ISENABLER + 4 * i, &enabled) || gic_read(GICD + 0x200 + 4 * i, &pending) ||
		    gic_read(GICD + 0x300 + 4 * i, &active))
			seq_printf(s, "interrupts %d-%d: read rejected\n", 32 * i, 32 * i + 31);
		else
			seq_printf(s, "interrupts %d-%d: enabled %#x, pending %#x, active %#x\n", 32 * i,
				   32 * i + 31, enabled, pending, active);
	}
	seq_printf(s, "queue to firmware: commands read %d written %d, data read %d written %d, len %d\n",
		   a->tx->cmd_s, a->tx->cmd_e, a->tx->data_s, a->tx->data_e, a->tx->data_len);
	seq_printf(s, "queue from firmware: commands read %d written %d, data read %d written %d, len %d%s\n",
		   a->rx->cmd_s, a->rx->cmd_e, a->rx->data_s, a->rx->data_e, a->rx->data_len,
		   a->queue_broken ? " (BROKEN)" : "");
	seq_printf(s, "messages: sent %u, received %u\n", a->sent, a->received);
	if (a->irq && !a->irq_disabled)
		seq_printf(s, "listening: on interrupt (%u so far)\n", a->interrupts);
	else
		seq_printf(s, "listening: polled every %u ms (interrupt %s)\n", POLL_MS,
			   a->irq ? "off: cannot be acknowledged, or it fired with nothing pending" : "not available");
	seq_printf(s, "capture: running %d, positions received %u, last %#x\n", a->capturing,
		   a->pointers, a->last_pointer);
	seq_printf(s, "WDMA4: status 0x3430 = %#x, control 0x3400 = %#x, start 0x3408 = %#x\n",
		   readl(a->sfr + ABOX_WDMA4_STATUS), readl(a->sfr + ABOX_WDMA4_CTRL),
		   readl(a->sfr + ABOX_WDMA4_BUF_STR));
	seq_printf(s, "UAIF0: status %#x, control %#x\n", readl(a->sfr + ABOX_UAIF0_STATUS),
		   readl(a->sfr + ABOX_UAIF0_CTRL0));
	seq_printf(s, "playback: running %d, positions received %u, last %#x, ran out of samples %u times, cleaned %zu, written %zu\n",
		   a->sound_started, a->sound_pointers, a->sound_last, a->sound_underruns, a->cleaned,
		   a->written);
	seq_printf(s, "RDMA0: status 0x1030 = %#x, control 0x1000 = %#x, start 0x1008 = %#x\n",
		   readl(a->sfr + ABOX_RDMA0_STATUS), readl(a->sfr + ABOX_RDMA0_CTRL),
		   readl(a->sfr + ABOX_RDMA0_BUF_STR));
	seq_printf(s, "UAIF1: status %#x, control %#x; routing 0x40 = %#x; bit clock %lu Hz\n",
		   readl(a->sfr + ABOX_UAIF1_STATUS), readl(a->sfr + ABOX_UAIF1_CTRL0),
		   readl(a->sfr + ABOX_ROUTE_CTRL0), clk_get_rate(a->select1));
	{
		/* The registers of the playback path, those the module writes and those of the firmware. */
		static const u16 reg[] = {
			ABOX_QCHANNEL_DISABLE, ABOX_ROUTE_CTRL0, ABOX_ROUTE_CTRL1, ABOX_SPUS_CTRL_FC0, ABOX_SPUS_CTRL1,
			0x208, 0x20c, 0x210, ABOX_SPUS_SBANK_RDMA0, ABOX_SPUS_SIFS_CNT0, ABOX_UAIF1_CTRL0,
			ABOX_UAIF1_CTRL1, ABOX_RDMA0_CTRL, 0x1004, ABOX_RDMA0_BUF_STR, 0x100c, 0x1010, 0x1014,
			0x1018, 0x101c, 0x1020, ABOX_RDMA0_BIT_CTRL0,
		};

		seq_puts(s, "playback, registers:");
		for (i = 0; i < ARRAY_SIZE(reg); i++)
			seq_printf(s, " %x=%x", reg[i], readl(a->sfr + reg[i]));
		seq_putc(s, '\n');
	}
	if (a->amp) {
		/* The registers the module writes, plus the status ones (0xfa, 0xfb) and the identity one (0xff). */
		static const u8 reg[] = {
			AMP_SYSTEM, 0x01, AMP_VOLUME, 0x0b, 0x0c, AMP_MUTE, AMP_OUTPUT, 0x11, 0x14, 0x37, 0x3b,
			0x3c, 0x3f, 0x8b, 0x8c, 0x8d, AMP_PLL, 0x8f, 0x90, 0x91, 0x92, 0x94, 0x95, 0x96, 0x97,
			0xa2, AMP_STATUS1, AMP_STATUS2, AMP_ID,
		};

		seq_puts(s, "amplifier:");
		for (i = 0; i < ARRAY_SIZE(reg); i++)
			seq_printf(s, " %02x=%02x", reg[i], i2c_smbus_read_byte_data(a->amp, reg[i]) & 0xff);
		seq_putc(s, '\n');
	} else {
		seq_puts(s, "amplifier: absent\n");
	}
	sysmmu_fault(a, text, sizeof(text));
	seq_printf(s, "sysmmu: %s\n", text);
	seq_printf(s, "memory: DRAM %pad, samples %pad, tables %pad %pad\n", &a->dram_phys, &a->pcm_phys,
		   &a->tab1_phys, &a->tab2_phys);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(status);

static int log_show(struct seq_file *s, void *v)
{
	struct abox *a = s->private;
	size_t pos;

	mutex_lock(&a->log_lock);
	pos = a->log_written % LOG_SIZE;
	if (a->log_written >= LOG_SIZE)
		seq_write(s, a->log_buf + pos, LOG_SIZE - pos);
	seq_write(s, a->log_buf, pos);
	mutex_unlock(&a->log_lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(log);

static ssize_t command_write(struct file *file, const char __user *from, size_t n, loff_t *pos)
{
	struct abox *a = file->private_data;
	char line[32] = {};
	int volume, ret;

	if (n >= sizeof(line))
		return -EINVAL;
	if (copy_from_user(line, from, n))
		return -EFAULT;
	if (sscanf(line, "volume %i", &volume) != 1)
		return -EINVAL;
	if (!a->amp)
		return -ENODEV;
	if (volume < AMP_VOLUME_MAX || volume > AMP_VOLUME_MIN)
		return -EINVAL;
	ret = amp_update(a, AMP_VOLUME, 0xff, volume);
	return ret < 0 ? ret : n;
}

static const struct file_operations command_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = command_write,
};

/* ---- Start and stop ---- */

static int get_clocks(struct abox *a)
{
	struct device_node *cmu = of_find_compatible_node(NULL, NULL, "samsung,exynos850-cmu-aud");
	int i, ret = 0;

	if (!cmu)
		return -ENODEV;
	a->audif = clock_get(cmu, CLK_DOUT_AUD_AUDIF);
	a->bclk = clock_get(cmu, CLK_DOUT_AUD_UAIF0);
	a->bclk1 = clock_get(cmu, CLK_DOUT_AUD_UAIF1);
	a->select1 = clock_get(cmu, CLK_MOUT_AUD_UAIF1);
	a->mclk = clock_get(cmu, CLK_DOUT_AUD_MCLK);
	a->cpu = clock_get(cmu, CLK_MOUT_AUD_CPU_HCH);
	for (i = 0; i < ARRAY_SIZE(audio_gates); i++) {
		a->gate[i] = clock_get(cmu, audio_gates[i]);
		if (IS_ERR(a->gate[i]))
			ret = dev_err_probe(a->dev, PTR_ERR(a->gate[i]), "audio clock %u missing\n", audio_gates[i]);
	}
	of_node_put(cmu);
	if (IS_ERR(a->audif) || IS_ERR(a->bclk) || IS_ERR(a->bclk1) || IS_ERR(a->select1) || IS_ERR(a->mclk) ||
	    IS_ERR(a->cpu))
		ret = dev_err_probe(a->dev, -ENODEV, "audio clocks missing\n");
	return ret;
}

static int gic_target(u32 irq, u32 target)
{
	u32 reg = GICD + GICD_ITARGETSR + (irq & ~3), shift = (irq & 3) * 8, v;
	int ret = gic_read(reg, &v);

	return ret ?: gic_write(reg, (v & ~(0xff << shift)) | (BIT(target) << shift));
}

/* What Samsung does at every power-up: the message towards core 0, the watchdog to us. */
static int gic_setup(void)
{
	u32 type = 0, group;
	int ret;

	/*
	 * The controller is not reset when the core is turned off: an interrupt the core was
	 * serving at that moment stays "active" and would not be delivered to it any more. If it is
	 * that of a heartbeat timer, the new firmware does not start. Everything that was left
	 * active or pending is removed.
	 */
	ret = gic_read(GICD + GICD_TYPER, &type);
	for (group = 0; !ret && group <= (type & 0x1f); group++) {
		ret = gic_write(GICD + GICD_ICACTIVER + 4 * group, ~0);
		ret = ret ?: gic_write(GICD + GICD_ICPENDR + 4 * group, ~0);
	}
	ret = ret ?: gic_write(GICD + GICD_CTLR, 1);

	ret = ret ?: gic_target(SGI_ABOX_MSG, TARGET_CORE0);
	ret = ret ?: gic_write(GICD + GICD_ISENABLER, BIT(SGI_ABOX_MSG));
	ret = ret ?: gic_target(IRQ_WDT, TARGET_AP);
	ret = ret ?: gic_write(GICD + GICD_ISENABLER + 4 * (IRQ_WDT / 32), BIT(IRQ_WDT % 32));
	ret = ret ?: gic_write(GICC + GICC_PMR, 0xff);
	ret = ret ?: gic_write(GICD + GICD_CTLR, 3);
	return ret ?: gic_write(GICC + GICC_CTLR, 3);
}

static int load_firmware(struct abox *a)
{
	size_t n = min_t(size_t, strlen(bootargs), BOOTARGS_SIZE - 1);
	int i;

	for (i = 0; i < ARRAY_SIZE(binaries); i++)
		if (binaries[i].offset + a->fw[i]->size > (binaries[i].in_dram ? DRAM_BYTES : SRAM_SIZE)) {
			dev_err(a->dev, "%s is too big (%zu bytes)\n", binaries[i].name, a->fw[i]->size);
			return -EFBIG;
		}
	/* Samsung writes it first, before touching the core's internal memory. */
	writel(0, a->sysreg + SYSREG_ARAM_CTRL);
	memset_io(a->sram, 0, SRAM_SIZE);
	for (i = 0; i < ARRAY_SIZE(binaries); i++) {
		if (binaries[i].in_dram)
			memcpy(a->dram + binaries[i].offset, a->fw[i]->data, a->fw[i]->size);
		else
			memcpy_toio(a->sram + binaries[i].offset, a->fw[i]->data, a->fw[i]->size);
		dev_info(a->dev, "%s: %zu bytes in %s at %#x\n", binaries[i].name, a->fw[i]->size,
			 binaries[i].in_dram ? "DRAM" : "SRAM", binaries[i].offset);
	}
	memcpy_toio(a->sram + SRAM_BOOTARGS, bootargs, n);
	if (readl(a->sram) != get_unaligned_le32(a->fw[0]->data)) {
		dev_err(a->dev, "the SRAM does not hold what is written to it (%#x)\n", readl(a->sram));
		return -EIO;
	}
	dev_info(a->dev, "firmware copied, SRAM read back, bootargs \"%.*s\"\n", (int)n, bootargs);
	return 0;
}

static int abox_start(struct abox *a)
{
	struct device *dev = a->dev;
	char text[320];
	u32 value;
	int i, ret;

	/* 1. Memory: all below 4 GB, zeroed, not cached (another processor reads it). */
	a->phase = "memory";
	ret = dma_coerce_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	a->dram = dma_alloc_coherent(dev, DRAM_BYTES, &a->dram_phys, GFP_KERNEL);
	a->pcm = dma_alloc_coherent(dev, PCM_BYTES, &a->pcm_phys, GFP_KERNEL);
	a->sound = dma_alloc_coherent(dev, PCM_BYTES, &a->sound_phys, GFP_KERNEL);
	a->tab1 = dma_alloc_coherent(dev, TAB1_BYTES, &a->tab1_phys, GFP_KERNEL);
	a->tab2 = dma_alloc_coherent(dev, TAB2_BYTES, &a->tab2_phys, GFP_KERNEL);
	if (!a->dram || !a->pcm || !a->sound || !a->tab1 || !a->tab2) {
		dev_err(dev, "not enough memory (20 MiB contiguous needed for the firmware)\n");
		return -ENOMEM;
	}
	if (!IS_ALIGNED(a->dram_phys, SZ_1M) || !IS_ALIGNED(a->tab1_phys, TAB1_BYTES)) {
		dev_err(dev, "memory not aligned: DRAM %pad (1 MiB needed), table %pad (16 KiB)\n",
			&a->dram_phys, &a->tab1_phys);
		return -EINVAL;
	}
	a->tx = a->dram + DRAM_QUEUE_TX;
	a->rx = a->dram + DRAM_QUEUE_RX;
	dev_info(dev, "memory: DRAM %pad, samples %pad, tables %pad %pad\n", &a->dram_phys, &a->pcm_phys,
		 &a->tab1_phys, &a->tab2_phys);

	/* 2. Sysmmu. */
	a->phase = "sysmmu";
	for (i = 0; i < ARRAY_SIZE(audio_gates); i++) {
		ret = clk_prepare_enable(a->gate[i]);
		if (ret)
			dev_warn(dev, "audio clock %u not opened (%d)\n", audio_gates[i], ret);
	}
	ret = sysmmu_enable(a);
	if (ret)
		return ret;
	sysmmu_fault(a, text, sizeof(text));
	dev_info(dev, "sysmmu on: DRAM at %#x, samples at %#x, sound at %#x, identity at %#x; %s\n", IOVA_DRAM,
		 IOVA_PCM, IOVA_SOUND, IOVA_CHIPID, text);

	/* 3. Firmware. */
	a->phase = "firmware";
	ret = load_firmware(a);
	if (ret)
		return ret;

	/*
	 * 4. Preparation. The audio PLL does not run at an exact multiple of the wanted
	 * frequencies: the quotient rounded up is requested, which leads to the wanted divider (PLL
	 * / 14, then / 48 and / 3).
	 */
	a->phase = "preparation";
	clk_set_rate(a->audif, DIV_ROUND_UP(clk_get_rate(clk_get_parent(a->audif)), AUDIF_DIVIDER));
	clk_set_rate(a->bclk, DIV_ROUND_UP(clk_get_rate(a->audif), BCLK_DIVIDER));
	clk_set_rate(a->mclk, DIV_ROUND_UP(clk_get_rate(a->audif), AUDIF_HZ / MCLK_HZ));
	/* The bit clock towards the amplifier is generated by the ABOX, it does not come from outside. */
	clk_set_parent(a->select1, a->bclk1);
	clk_set_rate(a->bclk1, DIV_ROUND_UP(clk_get_rate(a->audif), BCLK_DIVIDER));
	clock_check(a, "root", a->audif, AUDIF_HZ);
	clock_check(a, "bit", a->bclk, AUDIF_HZ / BCLK_DIVIDER);
	clock_check(a, "codec", a->mclk, MCLK_HZ);
	clock_check(a, "amplifier bit", a->select1, AUDIF_HZ / BCLK_DIVIDER);
	dev_info(dev, "clocks: root %lu, bit %lu, amplifier bit %lu, codec %lu, audio core %lu Hz\n",
		 clk_get_rate(a->audif), clk_get_rate(a->bclk), clk_get_rate(a->select1), clk_get_rate(a->mclk),
		 clk_get_rate(a->cpu));
	writel((readl(a->pins + GPB0_CON) & ~GPB0_MASK) | GPB0_AUDIO, a->pins + GPB0_CON);
	writel((readl(a->pins + GPB1_CON) & ~GPB1_MASK) | GPB1_AUDIO, a->pins + GPB1_CON);

	ret = gic_setup();
	if (ret) {
		dev_err(dev, "the secure firmware rejects writes to the ABOX controller\n");
		return ret;
	}
	a->gic_ready = true;
	a->gic_acks = true;
	abox_irq_get(a);
	ret = regmap_update_bits(a->pmu, PMU_PAD, PMU_PAD_NORMAL, PMU_PAD_NORMAL);
	if (ret)
		return ret;
	writel(1, a->sfr + ABOX_SYSPOWER_CTRL);
	ret = readl_poll_timeout(a->sfr + ABOX_SYSPOWER_STATUS, value, value & 1, 100, 20000);
	dev_info(dev, "interrupt controller ready; internal power: status %#x (%d)\n",
		 value, ret);
	if (ret)
		return ret;

	/*
	 * An expired timer keeps its interrupt line high until the firmware lowers it (bit 2 of the
	 * command register). If the core was turned off with the timers running, nobody did: the new
	 * firmware would restart them, but a line that is already high no longer gives the edge its
	 * controller waits for, and the heartbeat would never begin.
	 */
	for (i = 0; i < TIMER_N; i++)
		writel(TIMER_LOWER_IRQ, a->timer + TIMER_CTRL0(i));
	timer_text(a, text, sizeof(text));
	dev_info(dev, "timers before the release: %s\n", text);

	/* 5. Core release: one bit per core. From here the periodic work listens to the firmware. */
	a->phase = "core released, waiting for the firmware";
	a->release_ns = local_clock();
	a->core_released = true;
	ret = regmap_update_bits(a->pmu, PMU_CORE0_POWER, 1, 1);
	if (!ret)
		ret = regmap_update_bits(a->pmu, PMU_CORE1_POWER, 1, 1);
	if (ret)
		return ret;
	regmap_read(a->pmu, PMU_CORE0_STANDBY, &value);
	dev_info(dev, "cores released (the second too): standby %#x\n", value);

	/* 6. Messages. */
	schedule_delayed_work(&a->poll, msecs_to_jiffies(POLL_MS));
	if (!wait_event_timeout(a->waitq, a->boot_done, msecs_to_jiffies(BOOT_MS))) {
		sysmmu_fault(a, text, sizeof(text));
		regmap_read(a->pmu, PMU_CORE0_STANDBY, &value);
		dev_err(dev, "no \"boot done\" in %u ms: PC %#x, standby %#x, sysmmu: %s\n", BOOT_MS,
			readl(a->sfr + ABOX_PC_CORE0), value, text);
		return -ETIMEDOUT;
	}
	return 0;
}

/* Brings everything back to a safe state, from any point of the start. Does not turn clocks off. */
static void abox_stop(struct abox *a)
{
	bool hold = false;
	u32 value = 0;
	int i, ret;

	/* The capture closes by itself while the periodic work still listens; then it stops. */
	a->exiting = true;
	wake_up_all(&a->waitq);
	cancel_delayed_work_sync(&a->poll);
	if (a->irq) {
		free_irq(a->irq, a);
		a->irq = 0;
	}

	/*
	 * Before turning the core off its interrupts are removed and it is let finish the one it is
	 * serving: turned off halfway, its interface with the controller would stay at that
	 * priority, and from here it cannot be cleared. The firmware loaded afterwards would no
	 * longer receive the heartbeat (lowest priority of all), until the phone is rebooted.
	 */
	if (a->gic_ready) {
		gic_write(GICD + GICD_CTLR, 0);
		msleep(5);
	}
	if (a->core_released) {
		regmap_update_bits(a->pmu, PMU_CORE0_POWER, 1, 0);
		regmap_update_bits(a->pmu, PMU_CORE1_POWER, 1, 0);
		ret = regmap_read_poll_timeout(a->pmu, PMU_CORE0_STANDBY, value,
					       (value & PMU_CORE0_STOPPED) == PMU_CORE0_STOPPED, 1000, 100000);
		dev_info(a->dev, "audio core 0 off: standby %#x (%d)\n", value, ret);
		hold = ret;
		ret = regmap_read_poll_timeout(a->pmu, PMU_CORE1_STANDBY, value,
					       (value & PMU_CORE1_STOPPED) == PMU_CORE1_STOPPED, 1000, 100000);
		dev_info(a->dev, "audio core 1 off: standby %#x (%d)\n", value, ret);
		hold = hold || ret;
		/*
		 * A core that does not stop can still write into the firmware memory, and WDMA4 and
		 * RDMA0 read and write at the address the firmware gave them: with the sysmmu off that
		 * would become a physical address, that is kernel memory. All of them must be stopped.
		 */
		abox_update(a, ABOX_WDMA4_CTRL, DMA_ENABLE, 0);
		abox_update(a, ABOX_RDMA0_CTRL, DMA_ENABLE, 0);
		hold = hold ||
		       readl_poll_timeout(a->sfr + ABOX_WDMA4_STATUS, value, !(value & DMA_RUNNING),
					  1000, 100000) ||
		       readl_poll_timeout(a->sfr + ABOX_RDMA0_STATUS, value, !(value & DMA_RUNNING),
					  1000, 100000);
	}
	if (hold)
		dev_warn(a->dev, "a core, WDMA4 or RDMA0 does not stop: leaving the sysmmu on and not freeing the memory\n");
	else if (a->sysmmu_enabled)
		sysmmu_disable(a);

	if (a->amp)
		i2c_unregister_device(a->amp);
	if (a->amp_bus)
		i2c_put_adapter(a->amp_bus);
	for (i = 0; i < ARRAY_SIZE(binaries); i++)
		release_firmware(a->fw[i]);
	if (!hold) {
		if (a->dram)
			dma_free_coherent(a->dev, DRAM_BYTES, a->dram, a->dram_phys);
		if (a->pcm)
			dma_free_coherent(a->dev, PCM_BYTES, a->pcm, a->pcm_phys);
		if (a->sound)
			dma_free_coherent(a->dev, PCM_BYTES, a->sound, a->sound_phys);
		if (a->tab1)
			dma_free_coherent(a->dev, TAB1_BYTES, a->tab1, a->tab1_phys);
		if (a->tab2)
			dma_free_coherent(a->dev, TAB2_BYTES, a->tab2, a->tab2_phys);
	}
	/*
	 * The clocks stay on: they already were (the bootloader leaves them on), and turning them
	 * off hangs the phone.
	 */
	for (i = 0; i < ARRAY_SIZE(audio_gates); i++)
		if (!IS_ERR_OR_NULL(a->gate[i]))
			clk_put(a->gate[i]);
	if (!IS_ERR_OR_NULL(a->audif))
		clk_put(a->audif);
	if (!IS_ERR_OR_NULL(a->bclk))
		clk_put(a->bclk);
	if (!IS_ERR_OR_NULL(a->bclk1))
		clk_put(a->bclk1);
	if (!IS_ERR_OR_NULL(a->select1))
		clk_put(a->select1);
	if (!IS_ERR_OR_NULL(a->mclk))
		clk_put(a->mclk);
	if (!IS_ERR_OR_NULL(a->cpu))
		clk_put(a->cpu);
}

static int abox_acquire(struct abox *a)
{
	struct device *dev = a->dev;
	struct device_node *np;
	int i, ret;

	np = of_find_compatible_node(NULL, NULL, "samsung,exynos850-acpm-ipc");
	if (!np)
		return dev_err_probe(dev, -ENODEV, "ACPM node missing\n");
	a->acpm = devm_acpm_get_by_node(dev, np);
	of_node_put(np);
	if (IS_ERR(a->acpm))
		return dev_err_probe(dev, PTR_ERR(a->acpm), "ACPM not available\n");
	a->pmu = exynos_get_pmu_regmap();
	if (IS_ERR(a->pmu))
		return dev_err_probe(dev, PTR_ERR(a->pmu), "PMU not available\n");
	ret = get_clocks(a);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(binaries); i++) {
		ret = request_firmware(&a->fw[i], binaries[i].name, dev);
		if (ret)
			return dev_err_probe(dev, ret, "%s missing from /lib/firmware\n", binaries[i].name);
		if (a->fw[i]->size < 4)
			return dev_err_probe(dev, -EINVAL, "%s is empty\n", binaries[i].name);
	}

	a->sfr = devm_ioremap(dev, ABOX_BASE, ABOX_SIZE);
	a->sram = devm_ioremap(dev, SRAM_BASE, SRAM_SIZE);
	a->sysreg = devm_ioremap(dev, SYSREG_BASE, SZ_4K);
	a->timer = devm_ioremap(dev, TIMER_BASE, SZ_4K);
	a->pins = devm_ioremap(dev, PINS_BASE, 0x40);
	a->sysmmu = devm_ioremap(dev, SYSMMU_BASE, SYSMMU_SIZE);
	if (!a->sfr || !a->sram || !a->sysreg || !a->timer || !a->pins || !a->sysmmu)
		return -ENOMEM;
	return 0;
}

static int abox_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct abox *a;
	int ret;

	a = devm_kzalloc(dev, sizeof(*a), GFP_KERNEL);
	if (!a)
		return -ENOMEM;
	a->dev = dev;
	a->phase = "resources";
	platform_set_drvdata(pdev, a);
	mutex_init(&a->tx_lock);
	spin_lock_init(&a->update_lock);
	mutex_init(&a->sound_lock);
	mutex_init(&a->log_lock);
	mutex_init(&a->listen_lock);
	init_waitqueue_head(&a->waitq);
	INIT_DELAYED_WORK(&a->poll, abox_poll);

	ret = abox_acquire(a);
	ret = ret ?: abox_start(a);
	if (ret) {
		dev_err(dev, "stopped at phase \"%s\" (%d): putting everything back as it was\n", a->phase, ret);
		abox_stop(a);
		return ret;
	}

	a->dir = debugfs_create_dir("abox", NULL);
	debugfs_create_file("status", 0400, a->dir, a, &status_fops);
	debugfs_create_file("log", 0400, a->dir, a, &log_fops);
	debugfs_create_file("command", 0200, a->dir, a, &command_fops);

	a->mic.minor = MISC_DYNAMIC_MINOR;
	a->mic.name = "abox-mic";
	a->mic.fops = &mic_fops;
	a->mic_registered = !misc_register(&a->mic);
	if (!a->mic_registered)
		dev_warn(dev, "/dev/abox-mic not created\n");

	amp_setup(a);
	if (a->amp) {
		a->speaker.minor = MISC_DYNAMIC_MINOR;
		a->speaker.name = "abox-speaker";
		a->speaker.fops = &speaker_fops;
		a->speaker_registered = !misc_register(&a->speaker);
		if (!a->speaker_registered)
			dev_warn(dev, "/dev/abox-speaker not created\n");
	}
	return 0;
}

static void abox_remove(struct platform_device *pdev)
{
	struct abox *a = platform_get_drvdata(pdev);

	if (a->mic_registered)
		misc_deregister(&a->mic);
	if (a->speaker_registered)
		misc_deregister(&a->speaker);
	debugfs_remove_recursive(a->dir);
	abox_stop(a);
}

static struct platform_driver abox_driver = {
	.probe = abox_probe,
	.remove = abox_remove,
	.driver = { .name = "abox-mangos" },
};

static struct platform_device *abox_pdev;

static int __init abox_init(void)
{
	int ret;

	ret = platform_driver_register(&abox_driver);
	if (ret)
		return ret;
	abox_pdev = platform_device_register_simple("abox-mangos", PLATFORM_DEVID_NONE, NULL, 0);
	if (IS_ERR(abox_pdev)) {
		platform_driver_unregister(&abox_driver);
		return PTR_ERR(abox_pdev);
	}
	/* The probe runs inside the registration and its result does not reach here: no bound driver means it failed. */
	if (!abox_pdev->dev.driver) {
		platform_device_unregister(abox_pdev);
		platform_driver_unregister(&abox_driver);
		return -ENODEV;
	}
	return 0;
}
module_init(abox_init);

static void __exit abox_exit(void)
{
	platform_device_unregister(abox_pdev);
	platform_driver_unregister(&abox_driver);
}
module_exit(abox_exit);

MODULE_DESCRIPTION("Start of the M13 audio processor with the Samsung firmware: recording from the microphone and playback on the speaker");
MODULE_FIRMWARE("calliope_sram.bin");
MODULE_FIRMWARE("calliope_dram.bin");
MODULE_FIRMWARE("calliope_sram_2.bin");
MODULE_FIRMWARE("calliope_dram_2.bin");
MODULE_LICENSE("GPL");
