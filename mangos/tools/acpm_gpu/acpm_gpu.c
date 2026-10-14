// SPDX-License-Identifier: GPL-2.0-only
/*
 * GPU frequency of the M13.
 *
 * The bootloader leaves the GPU at its lowest frequency (377 MHz out of 1001) and the kernel
 * does not raise it. The frequency is changed by the power management firmware, which also
 * adjusts the voltage by itself: here it is requested through the ACPM clock "g3d". There is no
 * automatic scaling from panfrost (devfreq): it would need the frequency table in the device
 * tree.
 *
 * The module offers two things:
 *  - /sys/devices/platform/acpm-gpu/khz: the wanted frequency, chosen among those in
 *    allowed_khz. The bridge sets it: full speed with the screen on, minimum with the screen off.
 *  - a thermal cooling device named "gpu": when the chip heats up, the kernel lowers the cap one
 *    step at a time. The real frequency is the lower of the wanted one and the cap.
 */
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/sysfs.h>
#include <linux/thermal.h>
#include <dt-bindings/clock/samsung,exynos850-acpm.h>

/* Samsung's frequencies for the M13 (gpu_dvfs_table of the original device tree), highest first. */
static const unsigned long khz_table[] = { 1001000, 865000, 754000, 598000, 433000, 377000 };
#define STEPS		ARRAY_SIZE(khz_table)
#define MIN_STEP	(STEPS - 1)

struct acpm_gpu {
	struct clk *clk;
	struct thermal_cooling_device *cooling;
	struct mutex lock;		/* protects wanted, cap and the request to the firmware */
	unsigned int wanted;		/* index in khz_table */
	unsigned int cap;		/* index in khz_table: 0 means no limit */
};

/* Ask the firmware for the lower of the wanted frequency and the thermal cap. */
static int gpu_apply(struct acpm_gpu *g)
{
	unsigned int step = max(g->wanted, g->cap);

	lockdep_assert_held(&g->lock);
	return clk_set_rate(g->clk, khz_table[step] * 1000);
}

static ssize_t khz_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct acpm_gpu *g = dev_get_drvdata(dev);

	guard(mutex)(&g->lock);
	return sysfs_emit(buf, "%lu\n", khz_table[max(g->wanted, g->cap)]);
}

static ssize_t khz_store(struct device *dev, struct device_attribute *attr, const char *buf,
			 size_t count)
{
	struct acpm_gpu *g = dev_get_drvdata(dev);
	unsigned long khz;
	unsigned int i, previous;
	int ret;

	ret = kstrtoul(buf, 10, &khz);
	if (ret)
		return ret;
	for (i = 0; i < STEPS; i++)
		if (khz_table[i] == khz)
			break;
	if (i == STEPS)
		return -EINVAL;

	guard(mutex)(&g->lock);
	previous = g->wanted;
	g->wanted = i;
	ret = gpu_apply(g);
	if (ret)
		g->wanted = previous;
	return ret ? ret : count;
}
static DEVICE_ATTR_RW(khz);

static ssize_t allowed_khz_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	unsigned int i;
	int written = 0;

	for (i = 0; i < STEPS; i++)
		written += sysfs_emit_at(buf, written, "%lu%c", khz_table[i],
					 i == MIN_STEP ? '\n' : ' ');
	return written;
}
static DEVICE_ATTR_RO(allowed_khz);

static struct attribute *gpu_attrs[] = {
	&dev_attr_khz.attr,
	&dev_attr_allowed_khz.attr,
	NULL,
};
ATTRIBUTE_GROUPS(gpu);

static int cooling_get_max_state(struct thermal_cooling_device *cdev, unsigned long *state)
{
	*state = MIN_STEP;
	return 0;
}

static int cooling_get_cur_state(struct thermal_cooling_device *cdev, unsigned long *state)
{
	struct acpm_gpu *g = cdev->devdata;

	guard(mutex)(&g->lock);
	*state = g->cap;
	return 0;
}

static int cooling_set_cur_state(struct thermal_cooling_device *cdev, unsigned long state)
{
	struct acpm_gpu *g = cdev->devdata;
	unsigned int previous;
	int ret;

	if (state > MIN_STEP)
		return -EINVAL;

	guard(mutex)(&g->lock);
	previous = g->cap;
	g->cap = state;
	ret = gpu_apply(g);
	if (ret)
		g->cap = previous;
	return ret;
}

static const struct thermal_cooling_device_ops cooling_ops = {
	.get_max_state = cooling_get_max_state,
	.get_cur_state = cooling_get_cur_state,
	.set_cur_state = cooling_set_cur_state,
};

static int gpu_probe(struct platform_device *pdev)
{
	struct of_phandle_args clock_spec = { .args_count = 1,
					      .args = { EXYNOS850_CLK_ACPM_DVFS_G3D } };
	struct device *dev = &pdev->dev;
	struct acpm_gpu *g;
	int ret;

	g = devm_kzalloc(dev, sizeof(*g), GFP_KERNEL);
	if (!g)
		return -ENOMEM;
	mutex_init(&g->lock);
	g->wanted = MIN_STEP;
	platform_set_drvdata(pdev, g);

	/* If the ACPM clocks are not registered yet, the probe is deferred and retried. */
	clock_spec.np = of_find_compatible_node(NULL, NULL, "samsung,exynos850-acpm-ipc");
	if (!clock_spec.np)
		return dev_err_probe(dev, -ENODEV, "ACPM node missing\n");
	g->clk = of_clk_get_from_provider(&clock_spec);
	of_node_put(clock_spec.np);
	if (IS_ERR(g->clk))
		return dev_err_probe(dev, PTR_ERR(g->clk), "GPU clock missing\n");

	/*
	 * The firmware cannot report the current frequency. At boot it is the minimum: request it
	 * so that from here on what the module reports is the real one.
	 */
	scoped_guard(mutex, &g->lock)
		ret = gpu_apply(g);
	if (ret) {
		clk_put(g->clk);
		return dev_err_probe(dev, ret, "firmware rejects the frequency\n");
	}

	/* acpm_thermal looks the "gpu" name up to attach this cooling device to the GPU zone. */
	g->cooling = thermal_cooling_device_register("gpu", g, &cooling_ops);
	if (IS_ERR(g->cooling)) {
		clk_put(g->clk);
		return dev_err_probe(dev, PTR_ERR(g->cooling), "cooling device not registered\n");
	}
	return 0;
}

static void gpu_remove(struct platform_device *pdev)
{
	struct acpm_gpu *g = platform_get_drvdata(pdev);

	thermal_cooling_device_unregister(g->cooling);
	/* Leave the GPU as the bootloader leaves it. */
	scoped_guard(mutex, &g->lock) {
		g->wanted = MIN_STEP;
		gpu_apply(g);
	}
	clk_put(g->clk);
}

static struct platform_driver gpu_driver = {
	.probe = gpu_probe,
	.remove = gpu_remove,
	.driver = { .name = "acpm-gpu", .dev_groups = gpu_groups },
};

static struct platform_device *gpu_pdev;

/* The probe can be deferred: the module still loads and the driver core retries. */
static int __init gpu_init(void)
{
	int ret;

	ret = platform_driver_register(&gpu_driver);
	if (ret)
		return ret;
	gpu_pdev = platform_device_register_simple("acpm-gpu", PLATFORM_DEVID_NONE, NULL, 0);
	if (IS_ERR(gpu_pdev)) {
		platform_driver_unregister(&gpu_driver);
		return PTR_ERR(gpu_pdev);
	}
	return 0;
}
module_init(gpu_init);

static void __exit gpu_exit(void)
{
	platform_device_unregister(gpu_pdev);
	platform_driver_unregister(&gpu_driver);
}
module_exit(gpu_exit);

MODULE_DESCRIPTION("M13 GPU frequency from the ACPM firmware, with thermal cooling device");
MODULE_LICENSE("GPL");
