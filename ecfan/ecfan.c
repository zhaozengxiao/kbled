// SPDX-License-Identifier: GPL-2.0-only
/*
 * ecfan - Clevo/HASEE EC fan & temperature hwmon driver
 *
 * The EC exposes its RAM through a system-memory window at 0xFE0B0100
 * (DSDT: OperationRegion (RAM, SystemMemory, 0xFE0B0100, 0x0100)).
 * Verified live on HASEE NH5x_NH7xHP (Clevo NH5x/NH7x): the window
 * matches /sys/kernel/debug/ec/ec0/io byte-for-byte and updates in
 * real time, so fan/temp fields can be read directly without touching
 * EC I/O ports (no lock conflicts with the kernel EC driver).
 *
 * DSDT field offsets inside the RAM region:
 *   TMP   0x07   EC temperature (deg C)
 *   DUT1  0xCE   fan1 duty (percent)
 *   DUT2  0xCF   fan2 duty (percent)
 *   RPM1  0xD0   fan1 speed (16-bit, big-endian on the wire)
 *   RPM2  0xD2   fan2 speed (16-bit, big-endian)
 *   RPM4  0xD4   fan4 speed (16-bit) - unused on this chassis
 *   RPM3  0xE0   fan3 speed (16-bit) - unused on this chassis
 *
 * This driver registers hwmon sensors fan1_input, fan2_input, fan3_input
 * and temp1_input so GNOME Vitals, lm-sensors and other tools can see
 * the fans.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/err.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>

#define ECFAN_DRV_NAME "ecfan"
#define ECFAN_BASE     0xFE0B0100UL
#define ECFAN_SIZE     0x0100UL

#define REG_TMP  0x07
#define REG_RPM1 0xD0
#define REG_RPM2 0xD2
#define REG_RPM3 0xE0

static void __iomem *ecfan_mem;
static struct device *ecfan_hwmon_dev;
static struct platform_device *ecfan_pdev;
static DEFINE_MUTEX(ecfan_lock);

static u8 ecfan_rd8(u8 off)
{
	u8 v;

	mutex_lock(&ecfan_lock);
	v = readb(ecfan_mem + off);
	mutex_unlock(&ecfan_lock);
	return v;
}

/* DSDT 16-bit fields are big-endian on the wire */
static u16 ecfan_rd16(u8 off)
{
	return (u16)(((u16)ecfan_rd8(off) << 8) | ecfan_rd8(off + 1));
}

static int ecfan_read(struct device *dev, enum hwmon_sensor_types type,
		      u32 attr, int channel, long *val)
{
	switch (type) {
	case hwmon_fan:
		if (attr != hwmon_fan_input)
			return -EOPNOTSUPP;
		switch (channel) {
		case 0:
			*val = ecfan_rd16(REG_RPM1);
			break;
		case 1:
			*val = ecfan_rd16(REG_RPM2);
			break;
		case 2:
			*val = ecfan_rd16(REG_RPM3);
			break;
		default:
			return -EOPNOTSUPP;
		}
		return 0;
	case hwmon_temp:
		if (attr != hwmon_temp_input || channel != 0)
			return -EOPNOTSUPP;
		*val = ecfan_rd8(REG_TMP) * 1000;	/* millidegree C */
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static umode_t ecfan_is_visible(const void *drvdata,
				enum hwmon_sensor_types type,
				u32 attr, int channel)
{
	switch (type) {
	case hwmon_fan:
		if (attr == hwmon_fan_input && channel >= 0 && channel <= 2)
			return 0444;
		break;
	case hwmon_temp:
		if (attr == hwmon_temp_input && channel == 0)
			return 0444;
		break;
	default:
		break;
	}
	return 0;
}

static const struct hwmon_channel_info *const ecfan_info[] = {
	HWMON_CHANNEL_INFO(fan,
			   HWMON_F_INPUT,
			   HWMON_F_INPUT,
			   HWMON_F_INPUT),
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT),
	NULL
};

static const struct hwmon_ops ecfan_hwmon_ops = {
	.is_visible = ecfan_is_visible,
	.read = ecfan_read,
};

static const struct hwmon_chip_info ecfan_chip_info = {
	.ops = &ecfan_hwmon_ops,
	.info = ecfan_info,
};

static int __init ecfan_init(void)
{
	struct device *dev;
	int err;

	ecfan_pdev = platform_device_register_simple(ECFAN_DRV_NAME,
						     PLATFORM_DEVID_NONE,
						     NULL, 0);
	if (IS_ERR(ecfan_pdev)) {
		pr_err(ECFAN_DRV_NAME ": platform device failed: %ld\n",
		       PTR_ERR(ecfan_pdev));
		return PTR_ERR(ecfan_pdev);
	}
	dev = &ecfan_pdev->dev;

	ecfan_mem = ioremap(ECFAN_BASE, ECFAN_SIZE);
	if (!ecfan_mem) {
		pr_err(ECFAN_DRV_NAME ": cannot ioremap 0x%lx\n", ECFAN_BASE);
		err = -ENOMEM;
		goto err_pdev;
	}

	ecfan_hwmon_dev = hwmon_device_register_with_info(dev,
							  ECFAN_DRV_NAME,
							  NULL,
							  &ecfan_chip_info,
							  NULL);
	if (IS_ERR(ecfan_hwmon_dev)) {
		pr_err(ECFAN_DRV_NAME ": hwmon register failed: %ld\n",
		       PTR_ERR(ecfan_hwmon_dev));
		err = PTR_ERR(ecfan_hwmon_dev);
		goto err_unmap;
	}

	pr_info(ECFAN_DRV_NAME ": Clevo EC fan/temp sensor at 0x%lx\n",
		ECFAN_BASE);
	return 0;

err_unmap:
	iounmap(ecfan_mem);
err_pdev:
	platform_device_unregister(ecfan_pdev);
	return err;
}
module_init(ecfan_init);

static void __exit ecfan_exit(void)
{
	hwmon_device_unregister(ecfan_hwmon_dev);
	iounmap(ecfan_mem);
	platform_device_unregister(ecfan_pdev);
	pr_info(ECFAN_DRV_NAME ": unloaded\n");
}
module_exit(ecfan_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Clevo/HASEE EC fan and temperature hwmon driver");
MODULE_AUTHOR("zhaozengxiao");
