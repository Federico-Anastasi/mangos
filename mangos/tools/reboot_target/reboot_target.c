// SPDX-License-Identifier: GPL-2.0-only
/*
 * Reboot the M13 into the recovery partition or download mode, without key presses.
 *
 * The Samsung bootloader decides what to boot from a chip register that survives a reboot
 * (INFORM3): Samsung's kernel writes a prefix plus the wanted target there. This module does the
 * same, according to the argument the reboot was requested with:
 *
 *   recovery   boot the image in the recovery partition
 *   download   download mode (the one heimdall talks to)
 *
 * Any other reboot asks for a normal boot from the boot partition. This must be written every
 * time: if the register is left alone, the bootloader repeats the previous target (a kernel
 * started from recovery would come back to recovery). The values come from the Samsung M135F
 * sources (drivers/samsung/sec_reboot.c) and differ between models.
 *
 * Power off with the cable plugged in: the power chip does not cut power while a charger is
 * present, and the kernel would sit idle after shutdown. Like Samsung, if the phone is still on
 * after the power-off request, it is rebooted.
 */
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/reboot.h>
#include <linux/regmap.h>
#include <linux/soc/samsung/exynos-pmu.h>
#include <linux/string.h>

#define INFORM2			0x0808
#define INFORM3			0x080c
#define SYSIP_DAT0		0x0810
#define SYSTEM_CONFIGURATION	0x3a00
#define SWRESET_SYSTEM		BIT(1)
#define TARGET_PREFIX		0x12345600
#define TARGET_NORMAL		0
#define TARGET_DOWNLOAD		1
#define TARGET_RECOVERY		4
/*
 * INFORM2 bits 16 to 23: 0x4e tells the bootloader this is a requested reboot and not a
 * power-up caused by a charger. Samsung writes it on every reboot.
 */
#define INFORM2_MAGIC		(0x4e << 16)
#define INFORM2_MAGIC_MASK	(0xff << 16)

static struct regmap *pmu;

/* Write where to restart from at the next reboot; "normal" for any other name. */
static void reboot_write_target(const char *name)
{
	unsigned int target = TARGET_NORMAL;

	if (name && sysfs_streq(name, "recovery"))
		target = TARGET_RECOVERY;
	else if (name && sysfs_streq(name, "download"))
		target = TARGET_DOWNLOAD;

	regmap_update_bits(pmu, INFORM2, INFORM2_MAGIC_MASK, INFORM2_MAGIC);
	regmap_write(pmu, INFORM3, TARGET_PREFIX | target);
}

static int reboot_notify(struct notifier_block *nb, unsigned long action, void *cmd)
{
	if (action == SYS_RESTART)
		reboot_write_target(cmd);
	return NOTIFY_DONE;
}

static struct notifier_block reboot_nb = { .notifier_call = reboot_notify };

/*
 * Runs after the handler that cuts power: if we get here and are still on, power-off did not
 * work. With the cable plugged in this always happens, also for the thermal shutdown
 * (acpm_thermal): the phone restarts while the chip is still hot.
 */
static int poweroff_failed(struct sys_off_data *data)
{
	mdelay(1000);
	pr_emerg("reboot_target: power-off did not cut power (cable plugged in?): rebooting\n");
	reboot_write_target(NULL);
	regmap_update_bits(pmu, SYSTEM_CONFIGURATION, SWRESET_SYSTEM, SWRESET_SYSTEM);
	return NOTIFY_DONE;
}

static struct sys_off_handler *poweroff_handler;

static int __init reboot_target_init(void)
{
	unsigned int inform2 = 0, inform3 = 0, dat0 = 0;
	int ret;

	pmu = exynos_get_pmu_regmap();
	if (IS_ERR(pmu))
		return PTR_ERR(pmu);
	regmap_read(pmu, INFORM2, &inform2);
	regmap_read(pmu, INFORM3, &inform3);
	regmap_read(pmu, SYSIP_DAT0, &dat0);
	pr_info("reboot_target: at boot INFORM2 %#x, INFORM3 %#x, SYSIP_DAT0 %#x\n", inform2,
		inform3, dat0);
	poweroff_handler = register_sys_off_handler(SYS_OFF_MODE_POWER_OFF, SYS_OFF_PRIO_LOW, poweroff_failed, NULL);
	if (IS_ERR(poweroff_handler))
		return PTR_ERR(poweroff_handler);
	ret = register_reboot_notifier(&reboot_nb);
	if (ret)
		unregister_sys_off_handler(poweroff_handler);
	return ret;
}
module_init(reboot_target_init);

static void __exit reboot_target_exit(void)
{
	unregister_reboot_notifier(&reboot_nb);
	unregister_sys_off_handler(poweroff_handler);
}
module_exit(reboot_target_exit);

MODULE_DESCRIPTION("Reboot the M13 into recovery or download mode");
MODULE_LICENSE("GPL");
