// SPDX-License-Identifier: GPL-2.0-only
/*
 * Test of the ACPM frequency protocol on a running phone, without touching the boot.
 *
 * On load it sends the power management firmware a single request and prints the outcome in the
 * kernel log; then the module is unloaded and loaded again for the next one.
 *
 *   insmod acpm_test.ko channel=0 clock_id=2            reads the frequency
 *   insmod acpm_test.ko channel=0 clock_id=2 khz=1053000   sets it
 *
 *   insmod acpm_test.ko channel=9 zone_id=0             reads a temperature
 *   insmod acpm_test.ko channel=2 reg_page=7 reg=0 count=8
 *                                                        reads registers of the power chip
 *   insmod acpm_test.ko pmu=0x2080 count=2              reads CPU PMU registers
 *                                                        (always powered: safe to read)
 *   insmod acpm_test.ko mmio=0x14a50000 count=4         reads registers at a physical address.
 *                                                        If the block is not powered or has no
 *                                                        clock the kernel hangs: only with the
 *                                                        watchdog active.
 *
 * Clocks (dt-bindings/clock/samsung,exynos850-acpm.h): 2 is the first CPU cluster, 3 the second.
 * Temperature zones, from the Samsung device tree: 0 and 1 the two CPU clusters, 2 the GPU.
 * Pages of the s2mpu12 power chip (channel 2): 0 common, 1 power supplies, 2 clock, 3, 7 and 8
 * the aud3004x audio codec (OTP, digital, analog).
 *
 * The channel must come from the Samsung device tree or from a driver that already works: a
 * request sent on the channel of another protocol (for example the power chip's) would be read
 * as one of its commands.
 */
#include <linux/firmware/samsung/exynos-acpm-protocol.h>
#include <linux/arm-smccc.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/soc/samsung/exynos-pmu.h>

static unsigned int channel;
module_param(channel, uint, 0444);
static unsigned int clock_id = 2;
module_param(clock_id, uint, 0444);
static unsigned long khz;
module_param(khz, ulong, 0444);
static int zone_id = -1;
module_param(zone_id, int, 0444);
static int reg_page = -1;
module_param(reg_page, int, 0444);
static unsigned int reg;
module_param(reg, uint, 0444);
static unsigned int count = 1;
module_param(count, uint, 0444);
static int pmu = -1;
module_param(pmu, int, 0444);
static unsigned long mmio;
module_param(mmio, ulong, 0444);
/*
 * smc=<address>: reads a register through the secure firmware, as the Samsung kernel does for
 * blocks the CPU cannot touch directly (for example the interrupt controller of the audio
 * processor). An address that is not allowed only gives an error code.
 */
static unsigned long smc;
module_param(smc, ulong, 0444);

#define SMC_CMD_REG		((unsigned long)-101)
#define SMC_REG_ID_SFR_R(a)	((0x3UL << 30) | ((a) >> 2))

static struct platform_device *test_pdev;

static int acpm_test_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct acpm_handle *handle;
	struct device_node *np;
	unsigned long rate;
	ktime_t start;
	int ret;

	if (smc) {
		struct arm_smccc_res res;
		unsigned int i;

		for (i = 0; i < clamp(count, 1u, 16u); i++) {
			arm_smccc_smc(SMC_CMD_REG, SMC_REG_ID_SFR_R(smc + 4 * i), 0, 0, 0, 0, 0, 0, &res);
			dev_info(dev, "smc %#lx: result %ld, values %#lx %#lx %#lx\n", smc + 4 * i,
				 (long)res.a0, res.a1, res.a2, res.a3);
		}
		return 0;
	}

	if (mmio) {
		void __iomem *base = ioremap(mmio & PAGE_MASK, PAGE_SIZE);
		unsigned int i, offset = mmio & ~PAGE_MASK;

		if (!base)
			return -ENOMEM;
		for (i = 0; i < clamp(count, 1u, 16u) && offset + 4 * i < PAGE_SIZE; i++)
			dev_info(dev, "%#lx: %#x\n", mmio + 4 * i, readl(base + offset + 4 * i));
		iounmap(base);
		return 0;
	}

	if (pmu >= 0) {
		struct regmap *pmu_regmap = exynos_get_pmu_regmap();
		unsigned int i, value;

		if (IS_ERR(pmu_regmap))
			return dev_err_probe(dev, PTR_ERR(pmu_regmap), "PMU not available\n");
		for (i = 0; i < clamp(count, 1u, 16u); i++) {
			value = 0;
			ret = regmap_read(pmu_regmap, pmu + 4 * i, &value);
			dev_info(dev, "PMU %#x: %#x, result %d\n", pmu + 4 * i, value, ret);
		}
		return 0;
	}

	np = of_find_compatible_node(NULL, NULL, "samsung,exynos850-acpm-ipc");
	if (!np)
		return dev_err_probe(dev, -ENODEV, "ACPM node missing\n");
	handle = devm_acpm_get_by_node(dev, np);
	of_node_put(np);
	if (IS_ERR(handle))
		return dev_err_probe(dev, PTR_ERR(handle), "ACPM not available\n");

	start = ktime_get();
	if (reg_page >= 0) {
		u8 bytes[8] = {};

		/* The protocol reads at most eight registers per request. */
		count = clamp(count, 1u, 8u);
		/* The last zero is the chip: there is only one on the M13 bus. */
		ret = handle->ops->pmic.bulk_read(handle, channel, reg_page, reg, 0, count, bytes);
		dev_info(dev, "channel %u page %#x from %#x: %*ph, result %d\n", channel, reg_page,
			 reg, (int)count, bytes, ret);
	} else if (zone_id >= 0) {
		int degrees = 0;

		ret = handle->ops->tmu.read_temp(handle, channel, zone_id, &degrees);
		dev_info(dev, "channel %u zone %d: %d degrees, result %d, %lld us\n",
			 channel, zone_id, degrees, ret, ktime_us_delta(ktime_get(), start));
	} else if (khz) {
		ret = handle->ops->dvfs.set_rate(handle, channel, clock_id, khz * 1000);
		dev_info(dev, "channel %u clock %u: requested %lu kHz, result %d, %lld us\n",
			 channel, clock_id, khz, ret, ktime_us_delta(ktime_get(), start));
	} else {
		rate = handle->ops->dvfs.get_rate(handle, channel, clock_id);
		dev_info(dev, "channel %u clock %u: read %lu kHz (0 = error), %lld us\n",
			 channel, clock_id, rate / 1000, ktime_us_delta(ktime_get(), start));
	}
	return 0;
}

static struct platform_driver acpm_test_driver = {
	.probe = acpm_test_probe,
	.driver = { .name = "acpm-test" },
};

static int __init acpm_test_init(void)
{
	int ret;

	ret = platform_driver_register(&acpm_test_driver);
	if (ret)
		return ret;
	test_pdev = platform_device_register_simple("acpm-test", PLATFORM_DEVID_NONE, NULL, 0);
	if (IS_ERR(test_pdev)) {
		platform_driver_unregister(&acpm_test_driver);
		return PTR_ERR(test_pdev);
	}
	if (!test_pdev->dev.driver) {
		platform_device_unregister(test_pdev);
		platform_driver_unregister(&acpm_test_driver);
		return -ENODEV;
	}
	return 0;
}
module_init(acpm_test_init);

static void __exit acpm_test_exit(void)
{
	platform_device_unregister(test_pdev);
	platform_driver_unregister(&acpm_test_driver);
}
module_exit(acpm_test_exit);

MODULE_DESCRIPTION("Test of the Exynos 850 ACPM frequency protocol");
MODULE_LICENSE("GPL");
