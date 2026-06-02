/*
 * edgevib_iio.c — EdgeVib IIO Vibration Device Driver
 *
 * Creates an IIO device with 24 channels exposing ESP32 vibration feature
 * vectors (24×float32) via standard Linux IIO interfaces.
 *
 * Architecture:
 *   Go daemon (iio-d) polls TimescaleDB vibration_view every 2s
 *     → binary.Write(96 bytes) → /dev/edgevib-iio-inject (custom cdev)
 *       → copy_from_user → private scan_data
 *         → iio_trigger_fire() → trigger handler
 *           → iio_push_to_buffers() → kfifo buffer
 *             → /dev/iio:device0 readable by iio_readdev / hexdump
 *
 * sysfs (standard IIO):
 *   /sys/bus/iio/devices/iio:device0/in_accel_x_raw  (single channel read)
 *   /sys/bus/iio/devices/iio:device0/in_accel_x_scale
 *   /sys/bus/iio/devices/iio:device0/sampling_frequency
 *
 * sysfs (custom):
 *   /sys/bus/iio/devices/iio:device0/injection_count
 *   /sys/bus/iio/devices/iio:device0/last_injection_time_ms
 *
 * Usage:
 *   insmod edgevib_iio.ko
 *   echo <96 bytes> > /dev/edgevib-iio-inject
 *   iio_readdev -b 256 -s 96 iio:device0 | xxd
 *   cat /sys/bus/iio/devices/iio:device0/in_accel_x_raw
 *
 * SPDX-License-Identifier: GPL-2.0
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/iio/iio.h>
#include <linux/iio/buffer.h>
#include <linux/iio/trigger.h>
#include <linux/iio/trigger_consumer.h>
#include <linux/iio/sw_trigger.h>
#include <linux/iio/triggered_buffer.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/jiffies.h>
#include <linux/atomic.h>
#include <linux/string.h>

#define EDGEVIB_IIO_DEV_NAME    "edgevib-iio"
#define EDGEVIB_INJECT_DEV_NAME "edgevib-iio-inject"
#define EDGEVIB_IIO_NUM_CHAN    24
#define EDGEVIB_INJECT_SIZE     (24 * sizeof(float))  /* 96 bytes */

/* ---- Private data structure ---- */

struct edgevib_iio_priv {
	float scan_data[EDGEVIB_IIO_NUM_CHAN];	/* current 24-dim vector */
	u64   last_injection_jiffies;

	/* Character device for userspace injection */
	dev_t           cdev_num;
	struct cdev     cdev;
	struct class   *cdev_class;
	struct device  *cdev_device;

	/* Custom sysfs counters */
	atomic_t        injection_count;
	atomic64_t      last_injection_time_ms;

	/* IIO objects (set during probe) */
	struct iio_dev     *indio_dev;
	struct iio_trigger *trig;
};

/* Module-level pointer for sysfs attribute access */
static struct edgevib_iio_priv *g_priv;

/* ---- Forward declarations ---- */

static int edgevib_iio_read_raw(struct iio_dev *indio_dev,
				struct iio_chan_spec const *chan,
				int *val, int *val2, long mask);

static irqreturn_t edgevib_iio_trigger_handler(int irq, void *p);

static ssize_t edgevib_inject_write(struct file *filp,
				    const char __user *buf,
				    size_t count, loff_t *pos);

/* ---- IIO Channel Specification ----
 *
 * 24 channels, 1:1 aligned with ESP32 push_feature_vector():
 *
 *   chan 0  = rms_x          (IIO_ACCEL, indexed X)
 *   chan 1  = rms_y          (IIO_ACCEL, indexed Y)
 *   chan 2  = rms_z          (IIO_ACCEL, indexed Z)
 *   chan 3  = overall_rms    (IIO_ACCEL, indexed envelope)
 *   chan 4  = peak_freq_x    (IIO_VOLTAGE, extend_name "peak_freq_x")
 *   chan 5  = peak_amp_x     (IIO_VOLTAGE, extend_name "peak_amp_x")
 *   chan 6  = skewness_x     (IIO_VOLTAGE, extend_name "skewness_x")
 *   chan 7  = kurtosis_x     (IIO_VOLTAGE, extend_name "kurtosis_x")
 *   chan 8  = crest_factor_x (IIO_VOLTAGE, extend_name "crest_factor_x")
 *   chan 9-16  = band_energy_x[0..7] (IIO_VOLTAGE)
 *   chan 17 = peak_freq_y    (IIO_VOLTAGE, extend_name "peak_freq_y")
 *   chan 18 = peak_amp_y     (IIO_VOLTAGE, extend_name "peak_amp_y")
 *   chan 19 = crest_factor_y (IIO_VOLTAGE, extend_name "crest_factor_y")
 *   chan 20 = peak_freq_z    (IIO_VOLTAGE, extend_name "peak_freq_z")
 *   chan 21 = peak_amp_z     (IIO_VOLTAGE, extend_name "peak_amp_z")
 *   chan 22 = crest_factor_z (IIO_VOLTAGE, extend_name "crest_factor_z")
 *   chan 23 = temperature_c  (IIO_TEMP)
 *
 * All channels: 32-bit float LE, storedbits=32, realbits=32.
 */

/* Helper macro: ACCEL channel (channels 0-2) */
#define IIO_CHAN_ACCEL(_idx, _mod)					\
	{								\
		.type           = IIO_ACCEL,				\
		.modified       = 1,					\
		.channel2       = _mod,					\
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |		\
				      BIT(IIO_CHAN_INFO_SCALE),		\
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SAMP_FREQ), \
		.scan_index     = (_idx),				\
		.scan_type      = {					\
			.sign       = 's',				\
			.realbits   = 32,				\
			.storagebits = 32,				\
			.endianness = IIO_LE,				\
		},							\
	}

/* Helper macro: ACCEL envelope (channel 3, overall_rms) */
#define IIO_CHAN_ACCEL_ENVELOPE(_idx)					\
	{								\
		.type           = IIO_ACCEL,				\
		.indexed        = 1,					\
		.channel        = (_idx),				\
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |		\
				      BIT(IIO_CHAN_INFO_SCALE),		\
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SAMP_FREQ), \
		.scan_index     = (_idx),				\
		.scan_type      = {					\
			.sign       = 's',				\
			.realbits   = 32,				\
			.storagebits = 32,				\
			.endianness = IIO_LE,				\
		},							\
	}

/* Helper macro: VOLTAGE channel with extend_name (channels 4-22) */
#define IIO_CHAN_VOLTAGE_EXT(_idx, _extname)				\
	{								\
		.type           = IIO_VOLTAGE,				\
		.indexed        = 1,					\
		.channel        = (_idx),				\
		.extend_name    = (_extname),				\
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |		\
				      BIT(IIO_CHAN_INFO_SCALE),		\
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SAMP_FREQ), \
		.scan_index     = (_idx),				\
		.scan_type      = {					\
			.sign       = 's',				\
			.realbits   = 32,				\
			.storagebits = 32,				\
			.endianness = IIO_LE,				\
		},							\
	}

/* Helper macro: TEMP channel (channel 23) */
#define IIO_CHAN_TEMP(_idx)						\
	{								\
		.type           = IIO_TEMP,				\
		.indexed        = 1,					\
		.channel        = (_idx),				\
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |		\
				      BIT(IIO_CHAN_INFO_SCALE),		\
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SAMP_FREQ), \
		.scan_index     = (_idx),				\
		.scan_type      = {					\
			.sign       = 's',				\
			.realbits   = 32,				\
			.storagebits = 32,				\
			.endianness = IIO_LE,				\
		},							\
	}

static const struct iio_chan_spec edgevib_iio_channels[] = {
	/* Channels 0-2: ACCEL X/Y/Z (RMS vibration in mm/s²) */
	IIO_CHAN_ACCEL(0, IIO_MOD_X),
	IIO_CHAN_ACCEL(1, IIO_MOD_Y),
	IIO_CHAN_ACCEL(2, IIO_MOD_Z),
	/* Channel 3: overall_rms */
	IIO_CHAN_ACCEL_ENVELOPE(3),
	/* Channels 4-8: X-axis spectral features */
	IIO_CHAN_VOLTAGE_EXT(4,  "peak_freq_x"),
	IIO_CHAN_VOLTAGE_EXT(5,  "peak_amp_x"),
	IIO_CHAN_VOLTAGE_EXT(6,  "skewness_x"),
	IIO_CHAN_VOLTAGE_EXT(7,  "kurtosis_x"),
	IIO_CHAN_VOLTAGE_EXT(8,  "crest_factor_x"),
	/* Channels 9-16: X-axis band energy bins [0..7] */
	IIO_CHAN_VOLTAGE_EXT(9,  "band_energy_x0"),
	IIO_CHAN_VOLTAGE_EXT(10, "band_energy_x1"),
	IIO_CHAN_VOLTAGE_EXT(11, "band_energy_x2"),
	IIO_CHAN_VOLTAGE_EXT(12, "band_energy_x3"),
	IIO_CHAN_VOLTAGE_EXT(13, "band_energy_x4"),
	IIO_CHAN_VOLTAGE_EXT(14, "band_energy_x5"),
	IIO_CHAN_VOLTAGE_EXT(15, "band_energy_x6"),
	IIO_CHAN_VOLTAGE_EXT(16, "band_energy_x7"),
	/* Channels 17-19: Y-axis features */
	IIO_CHAN_VOLTAGE_EXT(17, "peak_freq_y"),
	IIO_CHAN_VOLTAGE_EXT(18, "peak_amp_y"),
	IIO_CHAN_VOLTAGE_EXT(19, "crest_factor_y"),
	/* Channels 20-22: Z-axis features */
	IIO_CHAN_VOLTAGE_EXT(20, "peak_freq_z"),
	IIO_CHAN_VOLTAGE_EXT(21, "peak_amp_z"),
	IIO_CHAN_VOLTAGE_EXT(22, "crest_factor_z"),
	/* Channel 23: temperature (°C) */
	IIO_CHAN_TEMP(23),
};

/* ---- IIO read_raw callback ----
 *
 * Responds to cat /sys/bus/iio/devices/iio:device0/in_accel_x_raw etc.
 * Returns int values. For float features we use SCALE factor to convert.
 */

static int edgevib_iio_read_raw(struct iio_dev *indio_dev,
				struct iio_chan_spec const *chan,
				int *val, int *val2, long mask)
{
	struct edgevib_iio_priv *priv = iio_priv(indio_dev);
	int idx = chan->scan_index;

	if (idx < 0 || idx >= EDGEVIB_IIO_NUM_CHAN)
		return -EINVAL;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		/*
		 * Convert float32 → int by multiplying by 1000 (preserve
		 * 3 decimal places).  Actual value = raw * scale.
		 * Example: 1.234 mm/s² → raw=1234, scale=0.001
		 */
		*val = (int)(priv->scan_data[idx] * 1000.0f);
		return IIO_VAL_INT;

	case IIO_CHAN_INFO_SCALE:
		/* scale = 0.001 → val=0, val2=1000000 (0.001 = 1000/1000000) */
		*val = 0;
		*val2 = 1000;	/* actually 0.001 = val + val2/1000000... */
		return IIO_VAL_INT_PLUS_MICRO;

	case IIO_CHAN_INFO_SAMP_FREQ:
		/* Sampling: 0.5 Hz (ESP32采集周期 2s) */
		*val = 0;
		*val2 = 500000;	/* 0.5 Hz = 500 mHz */
		return IIO_VAL_INT_PLUS_MICRO;

	default:
		return -EINVAL;
	}
}

/* ---- IIO Trigger Handler ----
 *
 * Called when iio_trigger_fire() is invoked from the cdev write path.
 * Pushes the current scan_data into the IIO kfifo buffer, making it
 * available to userspace consumers (iio_readdev, cat /dev/iio:device0).
 */

static irqreturn_t edgevib_iio_trigger_handler(int irq, void *p)
{
	struct iio_poll_func *pf = p;
	struct iio_dev *indio_dev = pf->indio_dev;
	struct edgevib_iio_priv *priv = iio_priv(indio_dev);
	/* Buffer for scan element: 24 × s32 (we push int32, not float) */
	s32 scan_buf[EDGEVIB_IIO_NUM_CHAN];
	int i;

	/*
	 * Convert float32 → int32 (×1000) and push to IIO buffer.
	 * IIO framework will copy this to the kfifo.
	 */
	for (i = 0; i < EDGEVIB_IIO_NUM_CHAN; i++)
		scan_buf[i] = (s32)(priv->scan_data[i] * 1000.0f);

	iio_push_to_buffers_with_timestamp(indio_dev, scan_buf,
					   pf->timestamp);

	iio_trigger_notify_done(indio_dev->trig);
	return IRQ_HANDLED;
}

/* ---- cdev file_operations ----
 *
 * write() is the injection point: daemon writes 96 bytes (24×float32)
 * to /dev/edgevib-iio-inject, we copy the data, fire the IIO trigger.
 */

static int edgevib_inject_open(struct inode *inode, struct file *filp)
{
	return 0;	/* always succeeds */
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

	/* Strict: must be exactly 96 bytes */
	if (count != EDGEVIB_INJECT_SIZE)
		return -EINVAL;

	/* Copy the 24×float32 vector from userspace */
	ret = copy_from_user(priv->scan_data, buf, EDGEVIB_INJECT_SIZE);
	if (ret)
		return -EFAULT;

	/* Update timestamps and counters */
	priv->last_injection_jiffies = jiffies;
	atomic_inc(&priv->injection_count);
	atomic64_set(&priv->last_injection_time_ms,
		     jiffies_to_msecs(jiffies));

	/* Fire the IIO trigger — this invokes the trigger_handler
	 * which pushes data into the kfifo buffer */
	if (priv->trig)
		iio_trigger_fire(priv->trig);

	return EDGEVIB_INJECT_SIZE;
}

static const struct file_operations edgevib_inject_fops = {
	.owner   = THIS_MODULE,
	.open    = edgevib_inject_open,
	.release = edgevib_inject_release,
	.write   = edgevib_inject_write,
};

/* ---- Custom sysfs attributes ----
 *
 * Two read-only attributes, matching D1's 2-attribute pattern.
 */

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

/* ---- IIO device info ---- */

static const struct iio_info edgevib_iio_info = {
	.read_raw  = edgevib_iio_read_raw,
	.attrs     = edgevib_iio_groups,
};

/* ---- IIO buffer setup ops (required for triggered_buffer) ---- */

static int edgevib_iio_buffer_postenable(struct iio_dev *indio_dev)
{
	return 0;
}

static int edgevib_iio_buffer_predisable(struct iio_dev *indio_dev)
{
	return 0;
}

static const struct iio_buffer_setup_ops edgevib_iio_buffer_ops = {
	.postenable  = edgevib_iio_buffer_postenable,
	.predisable  = edgevib_iio_buffer_predisable,
};

/* ---- Module lifecycle ---- */

static int __init edgevib_iio_init(void)
{
	struct edgevib_iio_priv *priv;
	struct iio_dev *indio_dev;
	int ret;

	/* Allocate IIO device */
	indio_dev = iio_device_alloc(sizeof(*priv));
	if (!indio_dev)
		return -ENOMEM;

	priv = iio_priv(indio_dev);
	priv->indio_dev = indio_dev;
	g_priv = priv;

	/* Initialize scan data to zero */
	memset(priv->scan_data, 0, sizeof(priv->scan_data));
	atomic_set(&priv->injection_count, 0);
	atomic64_set(&priv->last_injection_time_ms, 0);

	/* Configure IIO device */
	indio_dev->name       = EDGEVIB_IIO_DEV_NAME;
	indio_dev->info       = &edgevib_iio_info;
	indio_dev->channels   = edgevib_iio_channels;
	indio_dev->num_channels = ARRAY_SIZE(edgevib_iio_channels);
	indio_dev->modes      = INDIO_DIRECT_MODE | INDIO_BUFFER_TRIGGERED;

	/* Set up triggered buffer support */
	ret = iio_triggered_buffer_setup(indio_dev,
					 &iio_pollfunc_store_time,
					 edgevib_iio_trigger_handler,
					 &edgevib_iio_buffer_ops);
	if (ret) {
		pr_err("edgevib_iio: iio_triggered_buffer_setup failed (err=%d)\n", ret);
		goto err_free_iio;
	}

	/* Register IIO device */
	ret = iio_device_register(indio_dev);
	if (ret) {
		pr_err("edgevib_iio: iio_device_register failed (err=%d)\n", ret);
		goto err_cleanup_buffer;
	}

	/* --- Set up character device for injection --- */

	/* Allocate a device number dynamically */
	ret = alloc_chrdev_region(&priv->cdev_num, 0, 1,
				  EDGEVIB_INJECT_DEV_NAME);
	if (ret) {
		pr_err("edgevib_iio: alloc_chrdev_region failed (err=%d)\n", ret);
		goto err_unregister_iio;
	}

	/* Initialize and add cdev */
	cdev_init(&priv->cdev, &edgevib_inject_fops);
	priv->cdev.owner = THIS_MODULE;
	ret = cdev_add(&priv->cdev, priv->cdev_num, 1);
	if (ret) {
		pr_err("edgevib_iio: cdev_add failed (err=%d)\n", ret);
		goto err_unregister_chrdev;
	}

	/* Create device node in /dev/ */
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

	pr_info("edgevib_iio: loaded, IIO device=%s, inject=/dev/%s\n",
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
err_cleanup_buffer:
	iio_triggered_buffer_cleanup(indio_dev);
err_free_iio:
	iio_device_free(indio_dev);
	g_priv = NULL;
	return ret;
}

static void __exit edgevib_iio_exit(void)
{
	struct edgevib_iio_priv *priv = g_priv;
	struct iio_dev *indio_dev;

	if (!priv)
		return;

	indio_dev = priv->indio_dev;

	/* Tear down character device */
	if (priv->cdev_device)
		device_destroy(priv->cdev_class, priv->cdev_num);
	if (priv->cdev_class)
		class_destroy(priv->cdev_class);
	cdev_del(&priv->cdev);
	unregister_chrdev_region(priv->cdev_num, 1);

	/* Tear down IIO */
	iio_device_unregister(indio_dev);
	iio_triggered_buffer_cleanup(indio_dev);
	iio_device_free(indio_dev);

	g_priv = NULL;
	pr_info("edgevib_iio: unloaded\n");
}

module_init(edgevib_iio_init);
module_exit(edgevib_iio_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("EdgeVib Team");
MODULE_DESCRIPTION("EdgeVib IIO Vibration Device — 24-dim feature vector via IIO");
