// SPDX-License-Identifier: GPL-2.0-only
/*
 * M13 chip temperature and CPU throttling when it heats up.
 *
 * The three temperatures (the two CPU clusters and the GPU) are read by the power management
 * firmware, which is queried with the ACPM protocol. This module exposes them as thermal zones
 * and attaches the CPU frequency to them: above the threshold the kernel lowers it step by step
 * until the temperature drops; at the critical threshold it asks for a power-off. With a charger
 * plugged in, power-off does not cut power and the phone restarts (reboot_target handles it).
 *
 * The thermal zones are not in the device tree: the module creates them itself, on a running
 * phone.
 */
#include <linux/cpu_cooling.h>
#include <linux/cpufreq.h>
#include <linux/firmware/samsung/exynos-acpm-protocol.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/thermal.h>

/* ACPM temperature channel: Samsung device tree (acpm_tmu), the same as the GS101. */
#define TEMPERATURE_CHANNEL	9

/*
 * Samsung's thresholds for the M13 (thermal-zones of the original device tree): throttling
 * starts at 76 degrees, 115 is the limit beyond which the phone must be powered off.
 */
#define TRIP_THROTTLE		76000
#define TRIP_POWEROFF		115000
#define HYSTERESIS		5000

static const struct thermal_trip trips[] = {
	{ .temperature = TRIP_THROTTLE, .hysteresis = HYSTERESIS, .type = THERMAL_TRIP_PASSIVE },
	{ .temperature = TRIP_POWEROFF, .hysteresis = HYSTERESIS, .type = THERMAL_TRIP_CRITICAL },
};

/* In the order the firmware numbers them. */
static const char * const zone_names[] = { "chip-cpu1", "chip-cpu2", "chip-gpu" };
#define GPU_ZONE	2
/* First CPU of each cluster. */
static const unsigned int clusters[] = { 0, 4 };

struct thermal_zone_entry {
	struct acpm_handle *handle;
	struct thermal_zone_device *tzd;
	u8 number;
};

static struct thermal_zone_entry zones[ARRAY_SIZE(zone_names)];
static struct thermal_cooling_device *cpu_cooling[ARRAY_SIZE(clusters)];
static struct platform_device *thermal_pdev;

static int thermal_get_temp(struct thermal_zone_device *tz, int *temp)
{
	struct thermal_zone_entry *entry = thermal_zone_device_priv(tz);
	int degrees, ret;

	ret = entry->handle->ops->tmu.read_temp(entry->handle, TEMPERATURE_CHANNEL, entry->number,
						&degrees);
	if (ret)
		return ret;
	*temp = degrees * 1000;
	return 0;
}

/*
 * Whichever zone heats up, all CPUs slow down: they are in the same chip. The GPU zone also
 * lowers the GPU frequency, with the cooling device of acpm_gpu: it is found by the name "gpu",
 * which must match the one that module registers.
 */
static bool thermal_should_bind(struct thermal_zone_device *tz, const struct thermal_trip *trip,
				struct thermal_cooling_device *cdev, struct cooling_spec *spec)
{
	struct thermal_zone_entry *entry = thermal_zone_device_priv(tz);
	int i;

	if (trip->type != THERMAL_TRIP_PASSIVE)
		return false;
	for (i = 0; i < ARRAY_SIZE(cpu_cooling); i++)
		if (cdev == cpu_cooling[i])
			return true;
	return entry->number == GPU_ZONE && !strcmp(cdev->type, "gpu");
}

static const struct thermal_zone_device_ops thermal_ops = {
	.get_temp = thermal_get_temp,
	.should_bind = thermal_should_bind,
};

static void thermal_teardown(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(zones); i++) {
		if (zones[i].tzd)
			thermal_zone_device_unregister(zones[i].tzd);
		zones[i].tzd = NULL;
	}
	for (i = 0; i < ARRAY_SIZE(cpu_cooling); i++) {
		if (cpu_cooling[i])
			cpufreq_cooling_unregister(cpu_cooling[i]);
		cpu_cooling[i] = NULL;
	}
}

static int thermal_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct acpm_handle *handle;
	struct cpufreq_policy *policy;
	struct device_node *np;
	int i, ret, degrees;

	np = of_find_compatible_node(NULL, NULL, "samsung,exynos850-acpm-ipc");
	if (!np)
		return dev_err_probe(dev, -ENODEV, "ACPM node missing\n");
	handle = devm_acpm_get_by_node(dev, np);
	of_node_put(np);
	if (IS_ERR(handle))
		return dev_err_probe(dev, PTR_ERR(handle), "ACPM not available\n");

	ret = handle->ops->tmu.read_temp(handle, TEMPERATURE_CHANNEL, 0, &degrees);
	if (ret)
		return dev_err_probe(dev, ret, "firmware does not report the temperature\n");

	/* Without frequency scaling there is nothing to throttle: retry when it appears. */
	for (i = 0; i < ARRAY_SIZE(clusters); i++) {
		policy = cpufreq_cpu_get(clusters[i]);
		if (!policy) {
			thermal_teardown();
			return -EPROBE_DEFER;
		}
		cpu_cooling[i] = cpufreq_cooling_register(policy);
		cpufreq_cpu_put(policy);
		if (IS_ERR(cpu_cooling[i])) {
			ret = PTR_ERR(cpu_cooling[i]);
			cpu_cooling[i] = NULL;
			thermal_teardown();
			return dev_err_probe(dev, ret, "cooling device of cluster %d not registered\n", i);
		}
	}

	for (i = 0; i < ARRAY_SIZE(zones); i++) {
		zones[i].handle = handle;
		zones[i].number = i;
		zones[i].tzd = thermal_zone_device_register_with_trips(zone_names[i], trips,
								       ARRAY_SIZE(trips), &zones[i],
								       &thermal_ops, NULL, 250, 1000);
		if (IS_ERR(zones[i].tzd)) {
			ret = PTR_ERR(zones[i].tzd);
			zones[i].tzd = NULL;
			thermal_teardown();
			return dev_err_probe(dev, ret, "zone %s not registered\n", zone_names[i]);
		}
		ret = thermal_zone_device_enable(zones[i].tzd);
		if (ret) {
			thermal_teardown();
			return dev_err_probe(dev, ret, "zone %s not enabled\n", zone_names[i]);
		}
	}
	dev_info(dev, "chip temperature: %d degrees, throttles from %d, powers off at %d\n", degrees,
		 TRIP_THROTTLE / 1000, TRIP_POWEROFF / 1000);
	return 0;
}

static void thermal_remove(struct platform_device *pdev)
{
	thermal_teardown();
}

static struct platform_driver thermal_driver = {
	.probe = thermal_probe,
	.remove = thermal_remove,
	.driver = { .name = "acpm-thermal" },
};

/* The probe stays deferred until frequency scaling exists, and the driver core retries it. */
static int __init thermal_init(void)
{
	int ret;

	ret = platform_driver_register(&thermal_driver);
	if (ret)
		return ret;
	thermal_pdev = platform_device_register_simple("acpm-thermal", PLATFORM_DEVID_NONE, NULL, 0);
	if (IS_ERR(thermal_pdev)) {
		platform_driver_unregister(&thermal_driver);
		return PTR_ERR(thermal_pdev);
	}
	return 0;
}
module_init(thermal_init);

static void __exit thermal_exit(void)
{
	platform_device_unregister(thermal_pdev);
	platform_driver_unregister(&thermal_driver);
}
module_exit(thermal_exit);

MODULE_DESCRIPTION("M13 chip temperature from the ACPM firmware, with CPU throttling");
MODULE_LICENSE("GPL");
