/*
 * edgevib_iio.c — EdgeVib IIO Vibration Device Driver (Phase 1: sysfs-only)
 *
 * Creates an IIO device with 24 channels exposing ESP32 vibration feature
 * vectors via standard Linux IIO sysfs interface.
 *
 * Phase 1 (current): IIO device + sysfs read_raw + cdev injection.
 *   No trigger, no kfifo buffer — just sysfs single-channel reads.
 *   Data flows: Go daemon write(/dev/edgevib-iio-inject) → scan_data
 *              user reads: cat /sys/bus/iio/.../in_accel_x_raw
 *
 * Phase 2 (future): Add IIO trigger + kfifo buffer for iio_readdev support.
 *
 * sysfs (standard IIO):
 *   /sys/bus/iio/devices/iio:device0/in_accel_x_raw
 *   /sys/bus/iio/devices/iio:device0/in_accel_x_scale
 *   /sys/bus/iio/devices/iio:device0/sampling_frequency
 *
 * sysfs (custom):
 *   /sys/bus/iio/devices/iio:device0/injection_count
 *   /sys/bus/iio/devices/iio:device0/last_injection_time_ms
 *
 * Usage:
 *   insmod edgevib_iio.ko
 *   echo 96_bytes > /dev/edgevib-iio-inject
 *   cat /sys/bus/iio/devices/iio:device0/in_accel_x_raw
 *
 * SPDX-License-Identifier: GPL-2.0
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/iio/iio.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/jiffies.h>
#include <linux/atomic.h>
#include <linux/string.h>
#include <linux/version.h>

#define EDGEVIB_IIO_DEV_NAME    "edgevib-iio"
#define EDGEVIB_INJECT_DEV_NAME "edgevib-iio-inject"
#define EDGEVIB_IIO_NUM_CHAN    24
#define EDGEVIB_INJECT_SIZE     (24 * sizeof(s32))

/* ---- Private data ---- */

struct edgevib_iio_priv {
	s32 scan_data[EDGEVIB_IIO_NUM_CHAN];	/* float×1000, ARM64 no-FP safe */
	u64   last_injection_jiffies;

	/* Character device */
	dev_t           cdev_num;
	struct cdev     cdev;
	struct class   *cdev_class;
	struct device  *cdev_device;

	/* Custom sysfs counters */
	atomic_t        injection_count;
	atomic64_t      last_injection_time_ms;
};

static struct edgevib_iio_priv *g_priv;

/* ---- Forward declarations ---- */

static int edgevib_iio_read_raw(struct iio_dev *indio_dev,
				struct iio_chan_spec const *chan,
				int *val, int *val2, long mask);

/* ---- IIO channel macros ---- */

/* ACCEL channel with X/Y/Z modifier (chans 0-2) */
#define IIO_CHAN_ACCEL(_idx, _mod)					\
	{								\
		.type           = IIO_ACCEL,				\
		.modified       = 1,					\
		.channel2       = _mod,					\
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |		\
				      BIT(IIO_CHAN_INFO_SCALE),		\
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SAMP_FREQ),\
		.indexed        = 1,					\
	}

/* ACCEL envelope (chan 3, overall_rms) */
#define IIO_CHAN_ACCEL_ENV(_idx)					\
	{								\
		.type           = IIO_ACCEL,				\
		.indexed        = 1,					\
		.channel        = (_idx),				\
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |		\
				      BIT(IIO_CHAN_INFO_SCALE),		\
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SAMP_FREQ),\
	}

/* VOLTAGE channel with extend_name (chans 4-22) */
#define IIO_CHAN_VOLTAGE_EXT(_idx, _label)				\
	{								\
		.type           = IIO_VOLTAGE,				\
		.indexed        = 1,					\
		.channel        = (_idx),				\
		.extend_name    = (_label),				\
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |		\
				      BIT(IIO_CHAN_INFO_SCALE),		\
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SAMP_FREQ),\
	}

/* TEMP channel (chan 23) */
#define IIO_CHAN_TEMP(_idx)						\
	{								\
		.type           = IIO_TEMP,				\
		.indexed        = 1,					\
		.channel        = (_idx),				\
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |		\
				      BIT(IIO_CHAN_INFO_SCALE),		\
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SAMP_FREQ),\
	}

static const struct iio_chan_spec edgevib_iio_channels[] = {
	IIO_CHAN_ACCEL(0, IIO_MOD_X),        /* rms_x */
	IIO_CHAN_ACCEL(1, IIO_MOD_Y),        /* rms_y */
	IIO_CHAN_ACCEL(2, IIO_MOD_Z),        /* rms_z */
	IIO_CHAN_ACCEL_ENV(3),               /* overall_rms */
	IIO_CHAN_VOLTAGE_EXT(4,  "peak_freq_x"),
	IIO_CHAN_VOLTAGE_EXT(5,  "peak_amp_x"),
	IIO_CHAN_VOLTAGE_EXT(6,  "skewness_x"),
	IIO_CHAN_VOLTAGE_EXT(7,  "kurtosis_x"),
	IIO_CHAN_VOLTAGE_EXT(8,  "crest_factor_x"),
	IIO_CHAN_VOLTAGE_EXT(9,  "band_energy_x0"),
	IIO_CHAN_VOLTAGE_EXT(10, "band_energy_x1"),
	IIO_CHAN_VOLTAGE_EXT(11, "band_energy_x2"),
	IIO_CHAN_VOLTAGE_EXT(12, "band_energy_x3"),
	IIO_CHAN_VOLTAGE_EXT(13, "band_energy_x4"),
	IIO_CHAN_VOLTAGE_EXT(14, "band_energy_x5"),
	IIO_CHAN_VOLTAGE_EXT(15, "band_energy_x6"),
	IIO_CHAN_VOLTAGE_EXT(16, "band_energy_x7"),
	IIO_CHAN_VOLTAGE_EXT(17, "peak_freq_y"),
	IIO_CHAN_VOLTAGE_EXT(18, "peak_amp_y"),
	IIO_CHAN_VOLTAGE_EXT(19, "crest_factor_y"),
	IIO_CHAN_VOLTAGE_EXT(20, "peak_freq_z"),
	IIO_CHAN_VOLTAGE_EXT(21, "peak_amp_z"),
	IIO_CHAN_VOLTAGE_EXT(22, "crest_factor_z"),
	IIO_CHAN_TEMP(23),                   /* temperature_c */
};

/* ---- read_raw: respond to sysfs reads ---- */

static int edgevib_iio_read_raw(struct iio_dev *indio_dev,
				struct iio_chan_spec const *chan,
				int *val, int *val2, long mask)
{
	struct edgevib_iio_priv *priv = iio_priv(indio_dev);
	int idx = chan->channel;

	if (idx < 0 || idx >= EDGEVIB_IIO_NUM_CHAN)
		return -EINVAL;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		/* Data stored as s32 = float × 1000. Return directly. */
		*val = priv->scan_data[idx];
		return IIO_VAL_INT;

	case IIO_CHAN_INFO_SCALE:
		/* scale = 0.001 (raw × 0.001 = physical value) */
		*val = 0;
		*val2 = 1000;
		return IIO_VAL_INT_PLUS_MICRO;

	case IIO_CHAN_INFO_SAMP_FREQ:
		/* 0.5 Hz (2s ESP32 collection period) */
		*val = 0;
		*val2 = 500000;
		return IIO_VAL_INT_PLUS_MICRO;

	default:
		return -EINVAL;
	}
}

static const struct iio_info edgevib_iio_info = {
	.read_raw = edgevib_iio_read_raw,
};

/* ---- cdev file_operations ---- */

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
	struct edgevib_iio_priv *priv;
	int ret;

	if (!g_priv)
		return -ENODEV;
	priv = g_priv;

	if (count != EDGEVIB_INJECT_SIZE)
		return -EINVAL;

	ret = copy_from_user(priv->scan_data, buf, EDGEVIB_INJECT_SIZE);
	if (ret)
		return -EFAULT;

	priv->last_injection_jiffies = jiffies;
	atomic_inc(&priv->injection_count);
	atomic64_set(&priv->last_injection_time_ms,
		     jiffies_to_msecs(jiffies));

	return EDGEVIB_INJECT_SIZE;
}

static const struct file_operations edgevib_inject_fops = {
	.owner   = THIS_MODULE,
	.open    = edgevib_inject_open,
	.release = edgevib_inject_release,
	.write   = edgevib_inject_write,
};

/* ---- Custom sysfs attributes ---- */

static ssize_t injection_count_show(struct device *dev,
				    struct device_attribute *attr,
				    char *buf)
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

static struct attribute *edgevib_iio_attrs[] = {
	&dev_attr_injection_count.attr,
	&dev_attr_last_injection_time_ms.attr,
	NULL,
};
ATTRIBUTE_GROUPS(edgevib_iio);

/* ---- Module lifecycle ---- */

static int __init edgevib_iio_init(void)
{
	struct edgevib_iio_priv *priv;
	struct iio_dev *indio_dev;
	int ret;

	indio_dev = iio_device_alloc(NULL, sizeof(*priv));
	if (!indio_dev)
		return -ENOMEM;

	priv = iio_priv(indio_dev);
	g_priv = priv;

	memset(priv->scan_data, 0, sizeof(priv->scan_data));
	atomic_set(&priv->injection_count, 0);
	atomic64_set(&priv->last_injection_time_ms, 0);

	indio_dev->name         = EDGEVIB_IIO_DEV_NAME;
	indio_dev->info         = &edgevib_iio_info;
	indio_dev->channels     = edgevib_iio_channels;
	indio_dev->num_channels = ARRAY_SIZE(edgevib_iio_channels);
	indio_dev->modes        = INDIO_DIRECT_MODE;
	indio_dev->dev.groups   = edgevib_iio_groups;

	ret = iio_device_register(indio_dev);
	if (ret) {
		pr_err("edgevib_iio: iio_device_register failed (err=%d)\n", ret);
		goto err_free_iio;
	}
	priv->indio_dev = indio_dev;  /* set AFTER register succeeds */

	/* ---- Set up cdev for injection ---- */
	ret = alloc_chrdev_region(&priv->cdev_num, 0, 1,
				  EDGEVIB_INJECT_DEV_NAME);
	if (ret) {
		pr_err("edgevib_iio: alloc_chrdev_region failed (err=%d)\n", ret);
		goto err_unregister_iio;
	}

	cdev_init(&priv->cdev, &edgevib_inject_fops);
	priv->cdev.owner = THIS_MODULE;
	ret = cdev_add(&priv->cdev, priv->cdev_num, 1);
	if (ret) {
		pr_err("edgevib_iio: cdev_add failed (err=%d)\n", ret);
		goto err_unregister_chrdev;
	}

	priv->cdev_class = class_create(EDGEVIB_INJECT_DEV_NAME);
	if (IS_ERR(priv->cdev_class)) {
		ret = PTR_ERR(priv->cdev_class);
		pr_err("edgevib_iio: class_create failed (err=%d)\n", ret);
		goto err_del_cdev;
	}

	priv->cdev_device = device_create(priv->cdev_class, NULL,
					  priv->cdev_num, NULL,
					  EDGEVIB_INJECT_DEV_NAME);
	if (IS_ERR(priv->cdev_device)) {
		ret = PTR_ERR(priv->cdev_device);
		pr_err("edgevib_iio: device_create failed (err=%d)\n", ret);
		goto err_destroy_class;
	}

	pr_info("edgevib_iio: loaded (Phase 1 sysfs-only), IIO device=%s, inject=/dev/%s\n",
		EDGEVIB_IIO_DEV_NAME, EDGEVIB_INJECT_DEV_NAME);
	return 0;

err_destroy_class:
	class_destroy(priv->cdev_class);
err_del_cdev:
	cdev_del(&priv->cdev);
err_unregister_chrdev:
	unregister_chrdev_region(priv->cdev_num, 1);
err_unregister_iio:
	iio_device_unregister(indio_dev);
err_free_iio:
	iio_device_free(indio_dev);
	g_priv = NULL;
	return ret;
}

static void __exit edgevib_iio_exit(void)
{
	struct edgevib_iio_priv *priv = g_priv;

	if (!priv)
		return;

	if (priv->cdev_device)
		device_destroy(priv->cdev_class, priv->cdev_num);
	if (priv->cdev_class)
		class_destroy(priv->cdev_class);
	cdev_del(&priv->cdev);
	unregister_chrdev_region(priv->cdev_num, 1);

	iio_device_unregister(priv->indio_dev);
	iio_device_free(priv->indio_dev);

	g_priv = NULL;
	pr_info("edgevib_iio: unloaded\n");
}

module_init(edgevib_iio_init);
module_exit(edgevib_iio_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("EdgeVib Team");
MODULE_DESCRIPTION("EdgeVib IIO Vibration Device (Phase 1: sysfs-only)");
