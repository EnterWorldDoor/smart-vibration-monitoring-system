/*
 * edgevib_rtc.c — EdgeVib Software RTC Driver (Platform Driver)
 *
 * Registers a virtual RTC device via the kernel RTC subsystem. The RTC
 * maintains an independent time counter (rtc_base + jiffies offset) —
 * not a system clock mirror — mimicking real RTC hardware behaviour.
 *
 * Architecture:
 *   DT overlay (rtc-edgevib.dtbo) → platform bus match → probe()
 *     → devm_rtc_device_register() → /dev/rtcN
 *     → Custom sysfs on /sys/devices/platform/rtc-edgevib/
 *
 *   rtc-d (Go daemon):
 *     boot: read /var/lib/edgevib/last_time → ioctl(RTC_SET_TIME)
 *     runtime: ioctl(RTC_READ_TIME) → write file every 30s
 *     shutdown: SIGTERM → final ioctl(RTC_READ_TIME) → write file
 *     kernel: auto-set_time every 11 min (NTP→RTC sync)
 *
 * Alarm:
 *   hrtimer simulates hardware alarm interrupt → rtc_update_irq()
 *   Verified via: rtcwake -d /dev/rtcN -s 3 -m no
 *
 * rtc_class_ops implemented:
 *   read_time, set_time, read_alarm, set_alarm, alarm_irq_enable,
 *   read_offset, set_offset, proc
 *
 * Custom sysfs (under /sys/devices/platform/rtc-edgevib/):
 *   set_time_count        — number of set_time calls
 *   last_set_time_ms      — jiffies timestamp of last set_time
 *   alarm_fired_count     — number of alarm triggers
 *   last_alarm_time_ms    — jiffies timestamp of last alarm
 *
 * Usage:
 *   sudo dtoverlay rtc-edgevib       # Load DT overlay
 *   sudo insmod edgevib_rtc.ko       # Load module
 *   hwclock --show -f /dev/rtc0      # Read RTC
 *   hwclock --systohc -f /dev/rtc0   # Write system time to RTC
 *   rtcwake -d /dev/rtc0 -s 3 -m no  # Test alarm
 *
 * SPDX-License-Identifier: GPL-2.0
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/rtc.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/spinlock.h>
#include <linux/sysfs.h>
#include <linux/device.h>
#include <linux/atomic.h>
#include <linux/jiffies.h>
#include <linux/slab.h>
#include <linux/time64.h>

#define EDGEVIB_RTC_NAME  "rtc-edgevib"

/* ---- Private data ---- */

struct edgevib_rtc_priv {
	struct rtc_device   *rtc_dev;
	struct platform_device *pdev;
	struct hrtimer       alarm_timer;
	ktime_t              rtc_base;        /* RTC epoch: ktime at which RTC was set to 0 */
	bool                 alarm_enabled;
	ktime_t              alarm_target;    /* target ktime for alarm */
	spinlock_t           lock;

	/* Custom sysfs counters */
	atomic_t             set_time_count;
	atomic64_t           last_set_time_ms;
	atomic_t             alarm_fired_count;
	atomic64_t           last_alarm_time_ms;
};

/* ---- Forward declarations ---- */

static int edgevib_rtc_read_time(struct device *dev, struct rtc_time *tm);
static int edgevib_rtc_set_time(struct device *dev, struct rtc_time *tm);
static int edgevib_rtc_read_alarm(struct device *dev, struct rtc_wkalrm *alrm);
static int edgevib_rtc_set_alarm(struct device *dev, struct rtc_wkalrm *alrm);
static int edgevib_rtc_alarm_irq_enable(struct device *dev, unsigned int enabled);
static int edgevib_rtc_read_offset(struct device *dev, long *offset);
static int edgevib_rtc_set_offset(struct device *dev, long offset);
static int edgevib_rtc_proc(struct device *dev, struct seq_file *seq);
static enum hrtimer_restart edgevib_rtc_alarm_handler(struct hrtimer *timer);

/* ---- rtc_class_ops ---- */

static const struct rtc_class_ops edgevib_rtc_ops = {
	.read_time       = edgevib_rtc_read_time,
	.set_time        = edgevib_rtc_set_time,
	.read_alarm      = edgevib_rtc_read_alarm,
	.set_alarm       = edgevib_rtc_set_alarm,
	.alarm_irq_enable = edgevib_rtc_alarm_irq_enable,
	.read_offset     = edgevib_rtc_read_offset,
	.set_offset      = edgevib_rtc_set_offset,
	.proc            = edgevib_rtc_proc,
};

/* ---- read_time: convert rtc_base + elapsed → RTC time ---- */

static int edgevib_rtc_read_time(struct device *dev, struct rtc_time *tm)
{
	struct edgevib_rtc_priv *priv = dev_get_drvdata(dev);
	unsigned long flags;
	ktime_t now, elapsed;
	s64 elapsed_s;
	time64_t rtc_now;

	spin_lock_irqsave(&priv->lock, flags);
	now = ktime_get_real();
	elapsed = ktime_sub(now, priv->rtc_base);
	spin_unlock_irqrestore(&priv->lock, flags);

	elapsed_s = ktime_to_ns(elapsed);
	if (elapsed_s < 0)
		elapsed_s = 0;
	rtc_now = (time64_t)(elapsed_s / NSEC_PER_SEC);

	rtc_time64_to_tm(rtc_now, tm);

	return 0;
}

/* ---- set_time: recalibrate rtc_base ---- */

static int edgevib_rtc_set_time(struct device *dev, struct rtc_time *tm)
{
	struct edgevib_rtc_priv *priv = dev_get_drvdata(dev);
	time64_t new_time;
	ktime_t now;
	unsigned long flags;

	new_time = rtc_tm_to_time64(tm);

	spin_lock_irqsave(&priv->lock, flags);
	now = ktime_get_real();
	priv->rtc_base = ktime_sub_ns(now, new_time * NSEC_PER_SEC);
	spin_unlock_irqrestore(&priv->lock, flags);

	atomic_inc(&priv->set_time_count);
	atomic64_set(&priv->last_set_time_ms,
		     (long long)jiffies_to_msecs(jiffies));

	return 0;
}

/* ---- read_alarm: return stored alarm time ---- */

static int edgevib_rtc_read_alarm(struct device *dev, struct rtc_wkalrm *alrm)
{
	struct edgevib_rtc_priv *priv = dev_get_drvdata(dev);
	unsigned long flags;
	s64 alarm_s;

	spin_lock_irqsave(&priv->lock, flags);

	alrm->enabled = priv->alarm_enabled;
	alrm->pending = 0;

	if (priv->alarm_enabled && ktime_to_ns(priv->alarm_target) != 0) {
		alarm_s = ktime_to_ns(ktime_sub(priv->alarm_target, priv->rtc_base))
			  / NSEC_PER_SEC;
		if (alarm_s < 0)
			alarm_s = 0;
		rtc_time64_to_tm((time64_t)alarm_s, &alrm->time);
	}

	spin_unlock_irqrestore(&priv->lock, flags);
	return 0;
}

/* ---- set_alarm: store target + start hrtimer if enabled ---- */

static int edgevib_rtc_set_alarm(struct device *dev, struct rtc_wkalrm *alrm)
{
	struct edgevib_rtc_priv *priv = dev_get_drvdata(dev);
	time64_t alarm_time64;
	unsigned long flags;
	ktime_t target;

	alarm_time64 = rtc_tm_to_time64(&alrm->time);

	spin_lock_irqsave(&priv->lock, flags);

	/* Convert RTC time to real time */
	target = ktime_add_ns(priv->rtc_base, alarm_time64 * NSEC_PER_SEC);
	priv->alarm_target = target;

	if (alrm->enabled) {
		hrtimer_start(&priv->alarm_timer, target, HRTIMER_MODE_ABS);
		priv->alarm_enabled = true;
	} else {
		priv->alarm_target = target;  /* store but don't start */
	}

	spin_unlock_irqrestore(&priv->lock, flags);
	return 0;
}

/* ---- alarm_irq_enable: enable/disable hrtimer ---- */

static int edgevib_rtc_alarm_irq_enable(struct device *dev, unsigned int enabled)
{
	struct edgevib_rtc_priv *priv = dev_get_drvdata(dev);
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);

	priv->alarm_enabled = (bool)enabled;

	if (enabled && ktime_to_ns(priv->alarm_target) != 0) {
		hrtimer_start(&priv->alarm_timer, priv->alarm_target,
			      HRTIMER_MODE_ABS);
	} else {
		hrtimer_cancel(&priv->alarm_timer);
	}

	spin_unlock_irqrestore(&priv->lock, flags);
	return 0;
}

/* ---- read_offset: no independent oscillator → no offset ---- */

static int edgevib_rtc_read_offset(struct device *dev, long *offset)
{
	*offset = 0;
	return 0;
}

/* ---- set_offset: N/A — jiffies already follows NTP corrections ---- */

static int edgevib_rtc_set_offset(struct device *dev, long offset)
{
	/* Software RTC based on jiffies; NTP adjusts system clock directly.
	 * No independent oscillator to compensate.
	 */
	return 0;
}

/* ---- proc: expose custom fields in /proc/driver/rtc ---- */

static int edgevib_rtc_proc(struct device *dev, struct seq_file *seq)
{
	struct edgevib_rtc_priv *priv = dev_get_drvdata(dev);

	seq_printf(seq, "set_time_count    : %d\n",
		   atomic_read(&priv->set_time_count));
	seq_printf(seq, "alarm_fired_count : %d\n",
		   atomic_read(&priv->alarm_fired_count));

	return 0;
}

/* ---- hrtimer callback: fire alarm IRQ ---- */

static enum hrtimer_restart edgevib_rtc_alarm_handler(struct hrtimer *timer)
{
	struct edgevib_rtc_priv *priv =
		container_of(timer, struct edgevib_rtc_priv, alarm_timer);

	priv->alarm_enabled = false;
	atomic_inc(&priv->alarm_fired_count);
	atomic64_set(&priv->last_alarm_time_ms,
		     (long long)jiffies_to_msecs(jiffies));

	/* Signal alarm interrupt to RTC subsystem */
	rtc_update_irq(priv->rtc_dev, 1, RTC_AF | RTC_IRQF);

	return HRTIMER_NORESTART;
}

/* ---- Custom sysfs attributes ---- */

static ssize_t set_time_count_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct edgevib_rtc_priv *priv = dev_get_drvdata(dev);
	if (!priv)
		return -ENODEV;
	return sysfs_emit(buf, "%d\n", atomic_read(&priv->set_time_count));
}

static ssize_t last_set_time_ms_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct edgevib_rtc_priv *priv = dev_get_drvdata(dev);
	if (!priv)
		return -ENODEV;
	return sysfs_emit(buf, "%lld\n",
			  (long long)atomic64_read(&priv->last_set_time_ms));
}

static ssize_t alarm_fired_count_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct edgevib_rtc_priv *priv = dev_get_drvdata(dev);
	if (!priv)
		return -ENODEV;
	return sysfs_emit(buf, "%d\n", atomic_read(&priv->alarm_fired_count));
}

static ssize_t last_alarm_time_ms_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct edgevib_rtc_priv *priv = dev_get_drvdata(dev);
	if (!priv)
		return -ENODEV;
	return sysfs_emit(buf, "%lld\n",
			  (long long)atomic64_read(&priv->last_alarm_time_ms));
}

static DEVICE_ATTR_RO(set_time_count);
static DEVICE_ATTR_RO(last_set_time_ms);
static DEVICE_ATTR_RO(alarm_fired_count);
static DEVICE_ATTR_RO(last_alarm_time_ms);

static struct attribute *edgevib_rtc_sysfs_attrs[] = {
	&dev_attr_set_time_count.attr,
	&dev_attr_last_set_time_ms.attr,
	&dev_attr_alarm_fired_count.attr,
	&dev_attr_last_alarm_time_ms.attr,
	NULL,
};
ATTRIBUTE_GROUPS(edgevib_rtc_sysfs);

/* ---- platform_driver probe / remove ---- */

static int edgevib_rtc_probe(struct platform_device *pdev)
{
	struct edgevib_rtc_priv *priv;
	struct rtc_device *rtc;
	int ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->pdev = pdev;
	spin_lock_init(&priv->lock);

	/* Initialise counters */
	atomic_set(&priv->set_time_count, 0);
	atomic64_set(&priv->last_set_time_ms, 0);
	atomic_set(&priv->alarm_fired_count, 0);
	atomic64_set(&priv->last_alarm_time_ms, 0);

	/* Initialise RTC base to epoch 0 so read_time returns current system time.
	 * After the daemon calls set_time() the base is recalibrated.
	 * Writing rtc_base=0 means: read_time = (ktime_get_real() - 0) = current wall-clock.
	 * This prevents the kernel RTC subsystem from resetting the system clock to 1970
	 * on module load (the subsystem reads RTC at registration and may set system time).
	 */
	priv->rtc_base = ns_to_ktime(0);
	priv->alarm_enabled = false;
	priv->alarm_target = ns_to_ktime(0);

	/* Initialise hrtimer for alarm simulation */
	hrtimer_init(&priv->alarm_timer, CLOCK_REALTIME, HRTIMER_MODE_ABS);
	priv->alarm_timer.function = edgevib_rtc_alarm_handler;

	/* Register as platform driver data (before sysfs, so sysfs can read it) */
	platform_set_drvdata(pdev, priv);

	/* Attach custom sysfs attributes to platform device */
	ret = devm_device_add_groups(&pdev->dev, edgevib_rtc_sysfs_groups);
	if (ret) {
		dev_err(&pdev->dev, "sysfs group creation failed (err=%d)\n", ret);
		return ret;
	}

	/* Register RTC device */
	rtc = devm_rtc_device_register(&pdev->dev, EDGEVIB_RTC_NAME,
				       &edgevib_rtc_ops, THIS_MODULE);
	if (IS_ERR(rtc)) {
		ret = PTR_ERR(rtc);
		dev_err(&pdev->dev, "rtc_device_register failed (err=%d)\n", ret);
		return ret;
	}

	priv->rtc_dev = rtc;

	dev_info(&pdev->dev, "EdgeVib Software RTC loaded, dev=%s\n",
		 dev_name(&rtc->dev));

	return 0;
}

/* ---- of_match_table ---- */

static const struct of_device_id edgevib_rtc_of_match[] = {
	{ .compatible = "edgevib,rtc-edgevib" },
	{}
};
MODULE_DEVICE_TABLE(of, edgevib_rtc_of_match);

/* ---- platform_driver ---- */

static struct platform_driver edgevib_rtc_driver = {
	.probe  = edgevib_rtc_probe,
	.driver = {
		.name           = EDGEVIB_RTC_NAME,
		.owner          = THIS_MODULE,
		.of_match_table = edgevib_rtc_of_match,
	},
};

/* We use explicit init/exit instead of module_platform_driver() macro
 * so we can register a platform_device as fallback when DT overlay
 * is not available (CONFIG_OF_OVERLAY=n on this kernel).
 */
static struct platform_device *edgevib_rtc_pdev;

static int __init edgevib_rtc_init(void)
{
	int ret;

	/* Register platform device (fallback when DT overlay unavailable).
	 * If DT overlay IS loaded, the of_match_table / compatible match
	 * will also trigger probe(). The name-based match handles the
	 * no-DT case.
	 */
	edgevib_rtc_pdev = platform_device_register_simple(
		EDGEVIB_RTC_NAME, -1, NULL, 0);
	if (IS_ERR(edgevib_rtc_pdev)) {
		ret = PTR_ERR(edgevib_rtc_pdev);
		pr_err("edgevib_rtc: platform_device_register_simple failed (err=%d)\n", ret);
		return ret;
	}

	ret = platform_driver_register(&edgevib_rtc_driver);
	if (ret) {
		pr_err("edgevib_rtc: platform_driver_register failed (err=%d)\n", ret);
		platform_device_unregister(edgevib_rtc_pdev);
		return ret;
	}

	return 0;
}

static void __exit edgevib_rtc_exit(void)
{
	platform_driver_unregister(&edgevib_rtc_driver);
	platform_device_unregister(edgevib_rtc_pdev);
	pr_info("edgevib_rtc: unloaded\n");
}

module_init(edgevib_rtc_init);
module_exit(edgevib_rtc_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("EdgeVib Team");
MODULE_DESCRIPTION("EdgeVib Software RTC Driver — virtual RTC with hrtimer alarm");
