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
 *   RPM1  0xD0   fan1 tach count (16-bit, big-endian on the wire)
 *   RPM2  0xD2   fan2 tach count (16-bit, big-endian)
 *   RPM4  0xD4   fan4 tach count (16-bit) - unused on this chassis
 *   RPM3  0xE0   fan3 tach count (16-bit) - unused on this chassis
 *
 * This driver registers hwmon sensors fan1_input, fan2_input and
 * temp1_input so GNOME Vitals, lm-sensors and other tools can see
 * the fans (this chassis has exactly two fans).
 *
 * It also exposes power1_input = whole-platform power draw read from
 * the Intel RAPL PSYS MSR (0x64D), so Vitals can show system power.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/err.h>
#include <linux/mutex.h>
#include <linux/jiffies.h>
#include <linux/platform_device.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <asm/msr.h>

#define ECFAN_DRV_NAME "ecfan"
#define ECFAN_BASE     0xFE0B0100UL
#define ECFAN_SIZE     0x0100UL

#define REG_TMP  0x07
#define REG_RPM1 0xD0
#define REG_RPM2 0xD2

/*
 * EC fan fields are tachometer COUNTS, not RPM. Clevo firmware converts:
 *   RPM = ECFAN_TACH_DIV / count
 * Verified on this chassis: idle (count 966) -> 2232 RPM,
 * full load (count 489) -> 4409 RPM, which is physically sane
 * (the raw count falls as the fan spins faster).
 */
#define ECFAN_TACH_DIV 2156220UL

static void __iomem *ecfan_mem;
static struct device *ecfan_hwmon_dev;
static struct platform_device *ecfan_pdev;
static DEFINE_MUTEX(ecfan_lock);

/* RAPL PSYS power state */
static DEFINE_MUTEX(rapl_lock);
static u64 rapl_last_energy;
static unsigned long rapl_last_jiffies;
static int rapl_esu = 15;	/* energy unit exponent: 1/2^ESU joule */
static bool rapl_ok;
static long rapl_last_power;

static void rapl_init(void)
{
	u64 unit, e;

	if (rdmsrq_safe(MSR_RAPL_POWER_UNIT, &unit) == 0)
		rapl_esu = (int)((unit >> 8) & 0x1F);
	rapl_ok = (rdmsrq_safe(MSR_PLATFORM_ENERGY_STATUS, &e) == 0);
}

/*
 * Read whole-platform power in microwatts. Energy counter is 32-bit;
 * wraps around at 2^32 counts, handled by unsigned subtraction.
 * When the sampling window is too short (multiple readers), return the
 * last computed value instead of underestimating with a clamped window.
 */
static int ecfan_power_read(long *val)
{
	u64 e;
	unsigned long now, dt_ms;
	u32 de;

	if (!rapl_ok)
		return -ENODEV;
	if (rdmsrq_safe(MSR_PLATFORM_ENERGY_STATUS, &e))
		return -ENODEV;

	mutex_lock(&rapl_lock);
	now = jiffies;
	if (!rapl_last_jiffies) {
		rapl_last_energy = e;
		rapl_last_jiffies = now;
		*val = 0;
		mutex_unlock(&rapl_lock);
		return 0;
	}
	dt_ms = (now - rapl_last_jiffies) * 1000UL / HZ;
	de = (u32)(e - rapl_last_energy);
	rapl_last_energy = e;
	rapl_last_jiffies = now;
	mutex_unlock(&rapl_lock);

	if (dt_ms < 250) {
		*val = rapl_last_power;	/* window too short; reuse last value */
		return 0;
	}
	rapl_last_power = (long)((u64)de * 1000000000ULL /
				 (((u64)1 << rapl_esu) * dt_ms));
	*val = rapl_last_power;
	return 0;
}

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

/* EC stores a tachometer count; convert to RPM via ECFAN_TACH_DIV / count */
static u32 ecfan_rpm(u8 off)
{
	u16 count = ecfan_rd16(off);

	if (count == 0)
		return 0;	/* stalled / no fan */
	return (u32)(ECFAN_TACH_DIV / (u32)count);
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
			*val = ecfan_rpm(REG_RPM1);
			break;
		case 1:
			*val = ecfan_rpm(REG_RPM2);
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
	case hwmon_power:
		if (attr != hwmon_power_input || channel != 0)
			return -EOPNOTSUPP;
		return ecfan_power_read(val);
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
		if (attr == hwmon_fan_input && channel >= 0 && channel <= 1)
			return 0444;
		break;
	case hwmon_temp:
		if (attr == hwmon_temp_input && channel == 0)
			return 0444;
		break;
	case hwmon_power:
		if (attr == hwmon_power_input && channel == 0 && rapl_ok)
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
			   HWMON_F_INPUT),
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT),
	HWMON_CHANNEL_INFO(power, HWMON_P_INPUT),
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

	rapl_init();

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
