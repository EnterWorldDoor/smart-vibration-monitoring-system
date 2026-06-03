/*
 * edgevib_hwmon.c — EdgeVib HWMON Motor Health Driver
 *
 * Exposes motor electrical parameters (temperature, current, voltage, power)
 * through the standard Linux hwmon subsystem. Supports N motors via
 * num_motors module parameter.
 *
 * Architecture:
 *   edgevib-hwmon-d (Go daemon): TimescaleDB poll → /dev/edgevib-hwmon-inject
 *   hwmon sysfs: /sys/class/hwmon/hwmonN/temp1_input, curr1_input, etc.
 *
 * Standard hwmon sysfs (per motor):
 *   /sys/class/hwmon/hwmonN/temp1_input          — motor temperature (m°C)
 *   /sys/class/hwmon/hwmonN/temp1_max            — warning threshold
 *   /sys/class/hwmon/hwmonN/temp1_crit           — critical threshold
 *   /sys/class/hwmon/hwmonN/temp1_alarm          — 0=OK, 1=exceeded
 *   /sys/class/hwmon/hwmonN/curr1_input          — motor current (mA)
 *   /sys/class/hwmon/hwmonN/curr1_max            — warning threshold
 *   /sys/class/hwmon/hwmonN/curr1_alarm
 *   /sys/class/hwmon/hwmonN/in0_input            — bus voltage (mV)
 *   /sys/class/hwmon/hwmonN/in0_max
 *   /sys/class/hwmon/hwmonN/in0_alarm
 *   /sys/class/hwmon/hwmonN/power1_input         — motor power (mW)
 *   /sys/class/hwmon/hwmonN/power1_max
 *   /sys/class/hwmon/hwmonN/power1_alarm
 *
 * Custom sysfs (on inject cdev):
 *   /sys/devices/virtual/edgevib-hwmon-inject/injection_count
 *   /sys/devices/virtual/edgevib-hwmon-inject/last_injection_time_ms
 *
 * Usage:
 *   insmod edgevib_hwmon.ko num_motors=4
 *   python3 -c "import struct; ..." | dd of=/dev/edgevib-hwmon-inject bs=20 count=1
 *   sensors | grep motor01
 *
 * SPDX-License-Identifier: GPL-2.0
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/hwmon.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/jiffies.h>
#include <linux/atomic.h>
#include <linux/string.h>

#define EDGEVIB_HWMON_DEV_NAME    "edgevib-hwmon"
#define EDGEVIB_INJECT_DEV_NAME   "edgevib-hwmon-inject"
#define EDGEVIB_MAX_MOTORS        16
#define EDGEVIB_DEFAULT_MOTORS    4
#define EDGEVIB_INJECTION_SIZE    20	/* 5 × s32 (motor_id + temp + curr + volt + power) */

/* ---- Module parameter ---- */

static int num_motors = EDGEVIB_DEFAULT_MOTORS;
module_param(num_motors, int, 0444);
MODULE_PARM_DESC(num_motors, "Number of motors to monitor (1-16, default 4)");

/* ---- Data structures ---- */

/* Per-motor state — drvdata for each hwmon device */
struct edgevib_motor {
	char    name[16];		/* "motor01", "motor02", ... */
	bool    active;			/* daemon is injecting data */

	/* Electrical readings (s32 except power which needs >2.1GW µW range) */
	s32     temp_mC;		/* temperature (milli-Celsius) */
	s32     curr_mA;		/* current (milli-Amps) */
	s32     volt_mV;		/* voltage (milli-Volts) */
	long    power_uW;		/* power (micro-Watts) — long matches hwmon read/write */

	/* Warning / critical thresholds */
	s32     temp_max;		/* default 80000  = 80.0 °C */
	s32     temp_crit;		/* default 100000 = 100.0 °C */
	s32     curr_max;		/* default 10000  = 10.0 A */
	s32     volt_max;		/* default 400000 = 400.0 V (24V system + margin) */
	long    power_max;		/* default 2000000000 = 2.0 kW (µW, fits in s32 but uses long) */

	u64     last_injection_jiffies;
	struct device *hwmon_dev;	/* hwmon_device_register_with_info() return */
};

/* Binary injection struct — userspace writes this to cdev */
struct motor_injection {
	s32     motor_id;
	s32     temp_mC;
	s32     curr_mA;
	s32     volt_mV;
	s32     power_mW;
};

/* Top-level driver private data */
struct edgevib_hwmon_priv {
	struct edgevib_motor   *motors;
	int                     num_motors;

	/* cdev for data injection */
	dev_t                   cdev_num;
	struct cdev             cdev;
	struct class           *cdev_class;
	struct device          *cdev_device;

	/* Custom sysfs counters */
	atomic_t                injection_count;
	atomic64_t              last_injection_time_ms;
};

/* Global pointer — D2/D3 pattern */
static struct edgevib_hwmon_priv *g_priv;

/* ---- Forward declarations ---- */

static int edgevib_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			      u32 attr, int channel, long *val);
static int edgevib_hwmon_write(struct device *dev, enum hwmon_sensor_types type,
			       u32 attr, int channel, long val);
static umode_t edgevib_hwmon_is_visible(const void *drvdata,
					enum hwmon_sensor_types type,
					u32 attr, int channel);

/* ---- HWMON callbacks ---- */

static umode_t edgevib_hwmon_is_visible(const void *drvdata,
					enum hwmon_sensor_types type,
					u32 attr, int channel)
{
	const struct edgevib_motor *motor = drvdata;

	if (!motor->active)
		return 0;

	/*
	 * max / crit thresholds are writable; input / alarm are read-only.
	 * In the hwmon subsystem, _max attrs for different types (temp_max,
	 * curr_max, in_max, power_max) share the same integer value; only
	 * the outer `type` parameter disambiguates. So a single case covers
	 * all _max attrs. _crit is unique to temp.
	 */
	switch (attr) {
	case hwmon_temp_max:	/* also covers curr_max, in_max, power_max */
	case hwmon_temp_crit:
		return 0644;
	default:
		return 0444;
	}
}

static int edgevib_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			      u32 attr, int channel, long *val)
{
	struct edgevib_motor *motor = dev_get_drvdata(dev);

	if (!motor->active)
		return -ENODATA;

	switch (type) {
	case hwmon_temp:
		switch (attr) {
		case hwmon_temp_input:
			*val = motor->temp_mC;
			return 0;
		case hwmon_temp_max:
			*val = motor->temp_max;
			return 0;
		case hwmon_temp_crit:
			*val = motor->temp_crit;
			return 0;
		case hwmon_temp_alarm:
			*val = (motor->temp_mC >= motor->temp_max) ? 1 : 0;
			return 0;
		default:
			break;
		}
		break;
	case hwmon_curr:
		switch (attr) {
		case hwmon_curr_input:
			*val = motor->curr_mA;
			return 0;
		case hwmon_curr_max:
			*val = motor->curr_max;
			return 0;
		case hwmon_curr_alarm:
			*val = (motor->curr_mA >= motor->curr_max) ? 1 : 0;
			return 0;
		default:
			break;
		}
		break;
	case hwmon_in:
		switch (attr) {
		case hwmon_in_input:
			*val = motor->volt_mV;
			return 0;
		case hwmon_in_max:
			*val = motor->volt_max;
			return 0;
		case hwmon_in_alarm:
			*val = (motor->volt_mV >= motor->volt_max) ? 1 : 0;
			return 0;
		default:
			break;
		}
		break;
	case hwmon_power:
		switch (attr) {
		case hwmon_power_input:
			*val = motor->power_uW;
			return 0;
		case hwmon_power_max:
			*val = motor->power_max;
			return 0;
		case hwmon_power_alarm:
			*val = (motor->power_uW >= motor->power_max) ? 1 : 0;
			return 0;
		default:
			break;
		}
		break;
	default:
		break;
	}
	return -EOPNOTSUPP;
}

static int edgevib_hwmon_write(struct device *dev, enum hwmon_sensor_types type,
			       u32 attr, int channel, long val)
{
	struct edgevib_motor *motor = dev_get_drvdata(dev);

	switch (type) {
	case hwmon_temp:
		if (attr == hwmon_temp_max) {
			motor->temp_max = (s32)val;
			return 0;
		}
		if (attr == hwmon_temp_crit) {
			motor->temp_crit = (s32)val;
			return 0;
		}
		break;
	case hwmon_curr:
		if (attr == hwmon_curr_max) {
			motor->curr_max = (s32)val;
			return 0;
		}
		break;
	case hwmon_in:
		if (attr == hwmon_in_max) {
			motor->volt_max = (s32)val;
			return 0;
		}
		break;
	case hwmon_power:
		if (attr == hwmon_power_max) {
			motor->power_max = val;
			return 0;
		}
		break;
	default:
		break;
	}
	return -EOPNOTSUPP;
}

/* ---- HWMON ops and chip info ---- */

static const struct hwmon_ops edgevib_hwmon_ops = {
	.is_visible = edgevib_hwmon_is_visible,
	.read       = edgevib_hwmon_read,
	.write      = edgevib_hwmon_write,
};

/*
 * Channel configuration: 4 types × 1 channel each.
 * Each channel exposes input + max + alarm (temp also gets crit).
 * These bitmasks tell the hwmon framework which sysfs files to create.
 */
static const struct hwmon_channel_info *edgevib_hwmon_ch_info[] = {
	HWMON_CHANNEL_INFO(temp,
		HWMON_T_INPUT | HWMON_T_MAX | HWMON_T_CRIT | HWMON_T_ALARM),
	HWMON_CHANNEL_INFO(curr,
		HWMON_C_INPUT | HWMON_C_MAX | HWMON_C_ALARM),
	HWMON_CHANNEL_INFO(in,
		HWMON_I_INPUT | HWMON_I_MAX | HWMON_I_ALARM),
	HWMON_CHANNEL_INFO(power,
		HWMON_P_INPUT | HWMON_P_MAX | HWMON_P_ALARM),
	NULL
};

static const struct hwmon_chip_info edgevib_hwmon_chip_info = {
	.ops  = &edgevib_hwmon_ops,
	.info = edgevib_hwmon_ch_info,
};

/* ---- cdev file_operations (injection interface) ---- */

static int edgevib_inject_open(struct inode *inode, struct file *filp)
{
	return 0;
}

static int edgevib_inject_release(struct inode *inode, struct file *filp)
{
	return 0;
}

static ssize_t edgevib_inject_write(struct file *filp,
				    const char __user *buf,
				    size_t count, loff_t *pos)
{
	struct edgevib_hwmon_priv *priv;
	struct motor_injection inj;
	int ret;

	if (!g_priv)
		return -ENODEV;
	priv = g_priv;

	if (count != EDGEVIB_INJECTION_SIZE)
		return -EINVAL;

	ret = copy_from_user(&inj, buf, EDGEVIB_INJECTION_SIZE);
	if (ret)
		return -EFAULT;

	if (inj.motor_id < 0 || inj.motor_id >= priv->num_motors)
		return -EINVAL;

	if (!priv->motors[inj.motor_id].active)
		return -ENODEV;

	priv->motors[inj.motor_id].temp_mC  = inj.temp_mC;
	priv->motors[inj.motor_id].curr_mA  = inj.curr_mA;
	priv->motors[inj.motor_id].volt_mV  = inj.volt_mV;
	priv->motors[inj.motor_id].power_uW = (long)inj.power_mW;
	priv->motors[inj.motor_id].last_injection_jiffies = jiffies;

	atomic_inc(&priv->injection_count);
	atomic64_set(&priv->last_injection_time_ms,
		     (long long)jiffies_to_msecs(jiffies));

	return EDGEVIB_INJECTION_SIZE;
}

static const struct file_operations edgevib_inject_fops = {
	.owner   = THIS_MODULE,
	.open    = edgevib_inject_open,
	.release = edgevib_inject_release,
	.write   = edgevib_inject_write,
};

/* ---- Custom sysfs attributes (on cdev device) ---- */

static ssize_t injection_count_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	if (!g_priv)
		return -ENODEV;
	return sysfs_emit(buf, "%d\n", atomic_read(&g_priv->injection_count));
}

static ssize_t last_injection_time_ms_show(struct device *dev,
					   struct device_attribute *attr,
					   char *buf)
{
	if (!g_priv)
		return -ENODEV;
	return sysfs_emit(buf, "%lld\n",
			  (long long)atomic64_read(&g_priv->last_injection_time_ms));
}

static DEVICE_ATTR_RO(injection_count);
static DEVICE_ATTR_RO(last_injection_time_ms);

static struct attribute *edgevib_hwmon_sysfs_attrs[] = {
	&dev_attr_injection_count.attr,
	&dev_attr_last_injection_time_ms.attr,
	NULL,
};
ATTRIBUTE_GROUPS(edgevib_hwmon_sysfs);

/* ---- Module init / exit ---- */

static int __init edgevib_hwmon_init(void)
{
	struct edgevib_hwmon_priv *priv;
	int ret, i;

	/* Validate module param */
	if (num_motors < 1 || num_motors > EDGEVIB_MAX_MOTORS) {
		pr_err("edgevib_hwmon: num_motors must be 1-%d (got %d)\n",
		       EDGEVIB_MAX_MOTORS, num_motors);
		return -EINVAL;
	}

	/* Allocate private data + motor array */
	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	priv->num_motors = num_motors;

	priv->motors = kcalloc(num_motors, sizeof(struct edgevib_motor),
			       GFP_KERNEL);
	if (!priv->motors) {
		ret = -ENOMEM;
		goto err_free_priv;
	}

	/* Initialize each motor with defaults */
	for (i = 0; i < num_motors; i++) {
		snprintf(priv->motors[i].name, sizeof(priv->motors[i].name),
			 "motor%02d", i + 1);
		priv->motors[i].active    = true;
		priv->motors[i].temp_max  = 80000;	/* 80.0 °C */
		priv->motors[i].temp_crit = 100000;	/* 100.0 °C */
		priv->motors[i].curr_max  = 10000;	/* 10.0 A */
		priv->motors[i].volt_max  = 400000;	/* 400.0 V */
		priv->motors[i].power_max = 2000000000L;	/* 2.0 kW (µW) */
	}

	atomic_set(&priv->injection_count, 0);
	atomic64_set(&priv->last_injection_time_ms, 0);

	/* ---- Step 1: cdev registration (D2 pattern) ---- */
	ret = alloc_chrdev_region(&priv->cdev_num, 0, 1,
				  EDGEVIB_INJECT_DEV_NAME);
	if (ret) {
		pr_err("edgevib_hwmon: alloc_chrdev_region failed (err=%d)\n", ret);
		goto err_free_motors;
	}

	cdev_init(&priv->cdev, &edgevib_inject_fops);
	priv->cdev.owner = THIS_MODULE;
	ret = cdev_add(&priv->cdev, priv->cdev_num, 1);
	if (ret) {
		pr_err("edgevib_hwmon: cdev_add failed (err=%d)\n", ret);
		goto err_unregister_chrdev;
	}

	priv->cdev_class = class_create(EDGEVIB_INJECT_DEV_NAME);
	if (IS_ERR(priv->cdev_class)) {
		ret = PTR_ERR(priv->cdev_class);
		pr_err("edgevib_hwmon: class_create failed (err=%d)\n", ret);
		goto err_del_cdev;
	}

	priv->cdev_device = device_create_with_groups(
		priv->cdev_class, NULL, priv->cdev_num, priv,
		edgevib_hwmon_sysfs_groups, EDGEVIB_INJECT_DEV_NAME);
	if (IS_ERR(priv->cdev_device)) {
		ret = PTR_ERR(priv->cdev_device);
		pr_err("edgevib_hwmon: device_create_with_groups failed (err=%d)\n",
		       ret);
		goto err_destroy_class;
	}

	/* ---- Step 2: Register hwmon devices (one per motor) ---- */
	for (i = 0; i < num_motors; i++) {
		struct device *hwmon_dev;

		hwmon_dev = hwmon_device_register_with_info(
			priv->cdev_device,		/* parent */
			priv->motors[i].name,		/* "motor01", ... */
			&priv->motors[i],		/* drvdata */
			&edgevib_hwmon_chip_info,	/* ops + channels */
			NULL);				/* no extra groups */
		if (IS_ERR(hwmon_dev)) {
			ret = PTR_ERR(hwmon_dev);
			pr_err("edgevib_hwmon: hwmon_device_register failed for %s (err=%d)\n",
			       priv->motors[i].name, ret);
			goto err_unregister_hwmon;
		}
		priv->motors[i].hwmon_dev = hwmon_dev;
	}

	g_priv = priv;
	pr_info("edgevib_hwmon: loaded, %d motors, inject=/dev/%s\n",
		num_motors, EDGEVIB_INJECT_DEV_NAME);
	return 0;

	/* Error unwind — reverse order, goto chain (D2/D3 pattern) */
err_unregister_hwmon:
	for (i--; i >= 0; i--)
		hwmon_device_unregister(priv->motors[i].hwmon_dev);
	device_destroy(priv->cdev_class, priv->cdev_num);
err_destroy_class:
	class_destroy(priv->cdev_class);
err_del_cdev:
	cdev_del(&priv->cdev);
err_unregister_chrdev:
	unregister_chrdev_region(priv->cdev_num, 1);
err_free_motors:
	kfree(priv->motors);
err_free_priv:
	kfree(priv);
	return ret;
}

static void __exit edgevib_hwmon_exit(void)
{
	struct edgevib_hwmon_priv *priv = g_priv;
	int i;

	if (!priv)
		return;

	for (i = 0; i < priv->num_motors; i++)
		if (priv->motors[i].hwmon_dev)
			hwmon_device_unregister(priv->motors[i].hwmon_dev);

	device_destroy(priv->cdev_class, priv->cdev_num);
	class_destroy(priv->cdev_class);
	cdev_del(&priv->cdev);
	unregister_chrdev_region(priv->cdev_num, 1);

	kfree(priv->motors);
	kfree(priv);
	g_priv = NULL;

	pr_info("edgevib_hwmon: unloaded\n");
}

module_init(edgevib_hwmon_init);
module_exit(edgevib_hwmon_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("EdgeVib Team");
MODULE_DESCRIPTION("EdgeVib HWMON Motor Health Driver — workshop-level multi-motor electrical monitoring");
