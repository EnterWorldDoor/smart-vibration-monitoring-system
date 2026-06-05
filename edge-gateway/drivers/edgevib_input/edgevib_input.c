/*
 * edgevib_input.c — EdgeVib E-Stop Input Device Driver
 *
 * Registers a virtual input device (edgevib-estop) that exposes F407
 * emergency stop state as standard Linux evdev key events.
 *
 * Architecture:
 *   input-stop-d (Go daemon): MQTT subscribe → parse e_stop_state
 *     → write(struct input_event) to /dev/edgevib-input-inject
 *   edgevib_input.ko: cdev write handler → input_event() → evdev
 *
 * Key mapping (3-state via dual key):
 *   NORMAL     (0): KEY_STOP=0, KEY_WAKEUP=0
 *   EMERGENCY  (1): KEY_STOP=1, KEY_WAKEUP=0
 *   WAIT_RESET (2): KEY_STOP=0, KEY_WAKEUP=1
 *
 * Custom sysfs (on cdev device):
 *   /sys/devices/virtual/edgevib-input-inject/edgevib-input-inject/estop_event_count
 *   /sys/devices/virtual/edgevib-input-inject/edgevib-input-inject/last_estop_time_ms
 *
 * Usage:
 *   insmod edgevib_input.ko
 *   evtest /dev/input/eventX               # See KEY_STOP + KEY_WAKEUP
 *   # Inject KEY_STOP press from userspace:
 *   python3 -c "import struct,sys; ev=struct.pack('<iihhi',0,0,1,128,1); sys.stdout.buffer.write(ev)" | dd of=/dev/edgevib-input-inject bs=16 count=1
 *
 * SPDX-License-Identifier: GPL-2.0
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/jiffies.h>
#include <linux/atomic.h>
#include <linux/string.h>

#define EDGEVIB_INPUT_DEV_NAME      "edgevib-estop"
#define EDGEVIB_INJECT_DEV_NAME     "edgevib-input-inject"
#define EDGEVIB_INPUT_PHYS          "virtual/edgevib-estop/input0"
#define EDGEVIB_INPUT_VENDOR        0x0001
#define EDGEVIB_INPUT_PRODUCT       0x0006  /* D6 */

/* ---- Data structures ---- */

struct edgevib_input_priv {
	struct input_dev *input_dev;

	/* cdev for data injection */
	dev_t           cdev_num;
	struct cdev     cdev;
	struct class   *cdev_class;
	struct device  *cdev_device;

	/* Custom sysfs counters */
	atomic_t        estop_event_count;
	atomic64_t      last_estop_time_ms;
};

/* Global pointer — D2/D3/D5 pattern */
static struct edgevib_input_priv *g_priv;

/* ---- cdev file_operations (injection interface) ---- */

static int edgevib_inject_open(struct inode *inode, struct file *filp)
{
	return 0;
}

static int edgevib_inject_release(struct inode *inode, struct file *filp)
{
	return 0;
}

/*
 * edgevib_inject_write — Accept struct input_event (16 bytes) from userspace
 *
 * Validates type/code whitelist and passes the event to the input subsystem
 * via input_event(). This makes the event visible to evtest and any other
 * evdev consumer.
 */
static ssize_t edgevib_inject_write(struct file *filp,
				    const char __user *buf,
				    size_t count, loff_t *pos)
{
	struct edgevib_input_priv *priv;
	struct input_event ev;

	if (!g_priv)
		return -ENODEV;
	priv = g_priv;

	if (count != sizeof(struct input_event))
		return -EINVAL;

	if (copy_from_user(&ev, buf, sizeof(ev)))
		return -EFAULT;

	/* Whitelist: only allow EV_KEY with KEY_STOP or KEY_WAKEUP, or EV_SYN */
	if (ev.type != EV_KEY && ev.type != EV_SYN)
		return -EINVAL;

	if (ev.type == EV_KEY) {
		if (ev.code != KEY_STOP && ev.code != KEY_WAKEUP)
			return -EINVAL;
		if (ev.value != 0 && ev.value != 1 && ev.value != 2)
			return -EINVAL;

		/* Count KEY_STOP=1 events (EMERGENCY presses) */
		if (ev.code == KEY_STOP && ev.value == 1)
			atomic_inc(&priv->estop_event_count);
	}

	/* Forward to input subsystem — now visible in evtest */
	input_event(priv->input_dev, ev.type, ev.code, ev.value);

	atomic64_set(&priv->last_estop_time_ms,
		     (long long)jiffies_to_msecs(jiffies));

	return sizeof(ev);
}

static const struct file_operations edgevib_inject_fops = {
	.owner   = THIS_MODULE,
	.open    = edgevib_inject_open,
	.release = edgevib_inject_release,
	.write   = edgevib_inject_write,
};

/* ---- Custom sysfs attributes (on cdev device) ---- */

static ssize_t estop_event_count_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	if (!g_priv)
		return -ENODEV;
	return sysfs_emit(buf, "%d\n", atomic_read(&g_priv->estop_event_count));
}

static ssize_t last_estop_time_ms_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	if (!g_priv)
		return -ENODEV;
	return sysfs_emit(buf, "%lld\n",
			  (long long)atomic64_read(&g_priv->last_estop_time_ms));
}

static DEVICE_ATTR_RO(estop_event_count);
static DEVICE_ATTR_RO(last_estop_time_ms);

static struct attribute *edgevib_input_sysfs_attrs[] = {
	&dev_attr_estop_event_count.attr,
	&dev_attr_last_estop_time_ms.attr,
	NULL,
};
ATTRIBUTE_GROUPS(edgevib_input_sysfs);

/* ---- Module init / exit ---- */

static int __init edgevib_input_init(void)
{
	struct edgevib_input_priv *priv;
	int ret;

	/* Allocate private data */
	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	atomic_set(&priv->estop_event_count, 0);
	atomic64_set(&priv->last_estop_time_ms, 0);

	/* ---- Step 1: Register input device ---- */
	priv->input_dev = input_allocate_device();
	if (!priv->input_dev) {
		ret = -ENOMEM;
		goto err_free_priv;
	}

	priv->input_dev->name = EDGEVIB_INPUT_DEV_NAME;
	priv->input_dev->phys = EDGEVIB_INPUT_PHYS;
	priv->input_dev->id.bustype = BUS_VIRTUAL;
	priv->input_dev->id.vendor  = EDGEVIB_INPUT_VENDOR;
	priv->input_dev->id.product = EDGEVIB_INPUT_PRODUCT;
	priv->input_dev->id.version = 1;

	/* Set capability bits: EV_KEY + EV_SYN */
	set_bit(EV_KEY, priv->input_dev->evbit);
	set_bit(EV_SYN, priv->input_dev->evbit);
	set_bit(KEY_STOP, priv->input_dev->keybit);
	set_bit(KEY_WAKEUP, priv->input_dev->keybit);

	ret = input_register_device(priv->input_dev);
	if (ret) {
		pr_err("edgevib_input: input_register_device failed (err=%d)\n", ret);
		goto err_free_input;
	}

	/* ---- Step 2: cdev registration (D2/D5 pattern) ---- */
	ret = alloc_chrdev_region(&priv->cdev_num, 0, 1,
				  EDGEVIB_INJECT_DEV_NAME);
	if (ret) {
		pr_err("edgevib_input: alloc_chrdev_region failed (err=%d)\n", ret);
		goto err_unregister_input;
	}

	cdev_init(&priv->cdev, &edgevib_inject_fops);
	priv->cdev.owner = THIS_MODULE;
	ret = cdev_add(&priv->cdev, priv->cdev_num, 1);
	if (ret) {
		pr_err("edgevib_input: cdev_add failed (err=%d)\n", ret);
		goto err_unregister_chrdev;
	}

	priv->cdev_class = class_create(EDGEVIB_INJECT_DEV_NAME);
	if (IS_ERR(priv->cdev_class)) {
		ret = PTR_ERR(priv->cdev_class);
		pr_err("edgevib_input: class_create failed (err=%d)\n", ret);
		goto err_del_cdev;
	}

	priv->cdev_device = device_create_with_groups(
		priv->cdev_class, NULL, priv->cdev_num, priv,
		edgevib_input_sysfs_groups, EDGEVIB_INJECT_DEV_NAME);
	if (IS_ERR(priv->cdev_device)) {
		ret = PTR_ERR(priv->cdev_device);
		pr_err("edgevib_input: device_create_with_groups failed (err=%d)\n",
		       ret);
		goto err_destroy_class;
	}

	g_priv = priv;
	pr_info("edgevib_input: loaded, input=%s, inject=/dev/%s\n",
		EDGEVIB_INPUT_DEV_NAME, EDGEVIB_INJECT_DEV_NAME);
	return 0;

	/* Error unwind — reverse order (D2/D5 goto chain pattern) */
err_destroy_class:
	class_destroy(priv->cdev_class);
err_del_cdev:
	cdev_del(&priv->cdev);
err_unregister_chrdev:
	unregister_chrdev_region(priv->cdev_num, 1);
err_unregister_input:
	input_unregister_device(priv->input_dev);
	goto err_free_priv;
	/* input_free_device is not needed — input_unregister_device frees it
	 * because the device was allocated with input_allocate_device() and
	 * registered. On error before registration, we need to free separately. */
err_free_input:
	input_free_device(priv->input_dev);
err_free_priv:
	kfree(priv);
	return ret;
}

static void __exit edgevib_input_exit(void)
{
	struct edgevib_input_priv *priv = g_priv;

	if (!priv)
		return;

	/* Reverse order of init */
	if (priv->cdev_device && !IS_ERR(priv->cdev_device))
		device_destroy(priv->cdev_class, priv->cdev_num);
	if (priv->cdev_class)
		class_destroy(priv->cdev_class);
	cdev_del(&priv->cdev);
	unregister_chrdev_region(priv->cdev_num, 1);

	if (priv->input_dev) {
		input_unregister_device(priv->input_dev);
		/* input_unregister_device calls input_free_device internally */
	}

	kfree(priv);
	g_priv = NULL;

	pr_info("edgevib_input: unloaded\n");
}

module_init(edgevib_input_init);
module_exit(edgevib_input_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("EdgeVib Team");
MODULE_DESCRIPTION("EdgeVib E-Stop Input Device — exposes F407 emergency stop as standard evdev key events");
