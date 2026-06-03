/*
 * edgevib_gpio.c — EdgeVib Virtual GPIO Controller + IRQ Chip
 *
 * Registers a virtual gpio_chip with 6 lines (4 outputs + 2 inputs with IRQ)
 * for gateway-level health signaling and emergency input detection.
 *
 * Architecture:
 *   edgevib-gpio-d (Go daemon, systemd):
 *     MQTT subscribe → gpioset/gpiomon (subprocess) → /dev/gpiochipN
 *     Input IRQ: gpiomon line 4/5 → MQTT publish emergency events
 *
 * Lines:
 *   0: SYSTEM_OK       (output) — all core services healthy = high
 *   1: GATEWAY_ALERT   (output) — any service unhealthy = high
 *   2: HEARTBEAT       (output) — 1Hz square wave from daemon timer
 *   3: RESERVED        (output) — reserved for future use
 *   4: ESTOP_MASTER    (input + IRQ, falling edge) — emergency stop button
 *   5: PSU_FAIL_IN     (input + IRQ, rising edge)  — UPS mains failure
 *
 * sysfs (custom):
 *   /sys/devices/virtual/gpiochip/edgevib-gpio/irq_count
 *   /sys/devices/virtual/gpiochip/edgevib-gpio/last_irq_time_ms
 *   /sys/devices/virtual/gpiochip/edgevib-gpio/inject_irq
 *
 * Usage:
 *   insmod edgevib_gpio.ko
 *   gpiodetect | grep edgevib-gpio
 *   gpioinfo <chip>               # inspect lines
 *   gpioset <chip> 0=1            # set SYSTEM_OK high
 *   echo "4 0" > .../inject_irq   # simulate ESTOP falling edge
 *   gpiomon -r <chip> 4           # monitor ESTOP IRQ
 *
 * SPDX-License-Identifier: GPL-2.0
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/gpio/driver.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/sysfs.h>
#include <linux/atomic.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/jiffies.h>
#include <linux/device.h>

#define EDGEVIB_GPIO_NAME   "edgevib-gpio"
#define EDGEVIB_GPIO_NUM     6

/* ---- Private data ---- */

struct edgevib_gpio_priv {
	struct gpio_chip      chip;
	struct irq_chip       irq_chip;
	struct irq_domain    *irq_domain;
	struct device        *dev;          /* for sysfs groups */

	/* Line state */
	int                   line_values[EDGEVIB_GPIO_NUM];
	int                   line_direction[EDGEVIB_GPIO_NUM]; /* 0=in, 1=out */
	bool                  irq_masked[2]; /* offset 0=line4, 1=line5 */
	int                   irq_type[2];   /* IRQ_TYPE_EDGE_* or IRQ_TYPE_LEVEL_* */

	/* Sysfs counters */
	atomic_t              irq_count;
	atomic64_t            last_irq_time_ms;

	/* Lock */
	spinlock_t            lock;
};

static struct edgevib_gpio_priv *g_priv;

/* ---- Helpers ---- */

/* Map GPIO line index (4 or 5) to IRQ offset (0 or 1) */
static inline int line_to_irq_offset(unsigned int line)
{
	if (line < 4 || line > 5)
		return -EINVAL;
	return (int)(line - 4);
}

/* ---- gpio_chip ops ---- */

static int edgevib_gpio_get_direction(struct gpio_chip *chip, unsigned int offset)
{
	struct edgevib_gpio_priv *priv = gpiochip_get_data(chip);
	unsigned long flags;
	int dir;

	if (offset >= EDGEVIB_GPIO_NUM)
		return -EINVAL;

	spin_lock_irqsave(&priv->lock, flags);
	dir = priv->line_direction[offset];
	spin_unlock_irqrestore(&priv->lock, flags);

	return dir;
}

static int edgevib_gpio_direction_input(struct gpio_chip *chip, unsigned int offset)
{
	struct edgevib_gpio_priv *priv = gpiochip_get_data(chip);
	unsigned long flags;

	if (offset >= EDGEVIB_GPIO_NUM)
		return -EINVAL;

	spin_lock_irqsave(&priv->lock, flags);
	priv->line_direction[offset] = 0; /* input */
	spin_unlock_irqrestore(&priv->lock, flags);

	return 0;
}

static int edgevib_gpio_direction_output(struct gpio_chip *chip, unsigned int offset,
					  int value)
{
	struct edgevib_gpio_priv *priv = gpiochip_get_data(chip);
	unsigned long flags;

	if (offset >= EDGEVIB_GPIO_NUM)
		return -EINVAL;

	spin_lock_irqsave(&priv->lock, flags);
	priv->line_direction[offset] = 1; /* output */
	priv->line_values[offset] = value ? 1 : 0;
	spin_unlock_irqrestore(&priv->lock, flags);

	return 0;
}

static int edgevib_gpio_get(struct gpio_chip *chip, unsigned int offset)
{
	struct edgevib_gpio_priv *priv = gpiochip_get_data(chip);
	unsigned long flags;
	int val;

	if (offset >= EDGEVIB_GPIO_NUM)
		return -EINVAL;

	spin_lock_irqsave(&priv->lock, flags);
	val = priv->line_values[offset];
	spin_unlock_irqrestore(&priv->lock, flags);

	return val;
}

static void edgevib_gpio_set(struct gpio_chip *chip, unsigned int offset, int value)
{
	struct edgevib_gpio_priv *priv = gpiochip_get_data(chip);
	unsigned long flags;

	if (offset >= EDGEVIB_GPIO_NUM)
		return;

	spin_lock_irqsave(&priv->lock, flags);
	priv->line_values[offset] = value ? 1 : 0;
	spin_unlock_irqrestore(&priv->lock, flags);
}

static int edgevib_gpio_to_irq(struct gpio_chip *chip, unsigned int offset)
{
	struct edgevib_gpio_priv *priv = gpiochip_get_data(chip);
	int irq_offset;

	irq_offset = line_to_irq_offset(offset);
	if (irq_offset < 0)
		return -ENXIO;

	return irq_create_mapping(priv->irq_domain, irq_offset);
}

/* ---- irq_chip ops ---- */

static void edgevib_irq_mask(struct irq_data *data)
{
	struct edgevib_gpio_priv *priv = irq_data_get_irq_chip_data(data);
	unsigned int offset = irqd_to_hwirq(data);
	unsigned long flags;

	if (offset >= 2)
		return;

	spin_lock_irqsave(&priv->lock, flags);
	priv->irq_masked[offset] = true;
	spin_unlock_irqrestore(&priv->lock, flags);
}

static void edgevib_irq_unmask(struct irq_data *data)
{
	struct edgevib_gpio_priv *priv = irq_data_get_irq_chip_data(data);
	unsigned int offset = irqd_to_hwirq(data);
	unsigned long flags;

	if (offset >= 2)
		return;

	spin_lock_irqsave(&priv->lock, flags);
	priv->irq_masked[offset] = false;
	spin_unlock_irqrestore(&priv->lock, flags);
}

static int edgevib_irq_set_type(struct irq_data *data, unsigned int type)
{
	struct edgevib_gpio_priv *priv = irq_data_get_irq_chip_data(data);
	unsigned int offset = irqd_to_hwirq(data);
	unsigned long flags;

	if (offset >= 2)
		return -EINVAL;

	/* Accept edge (rising/falling/both) and level (high/low) triggers */
	if (!(type & (IRQ_TYPE_EDGE_RISING | IRQ_TYPE_EDGE_FALLING |
		      IRQ_TYPE_LEVEL_HIGH | IRQ_TYPE_LEVEL_LOW)))
		return -EINVAL;

	spin_lock_irqsave(&priv->lock, flags);
	priv->irq_type[offset] = type;
	spin_unlock_irqrestore(&priv->lock, flags);

	return 0;
}

static void edgevib_irq_bus_lock(struct irq_data *data)
{
	/* Virtual device: no hardware bus to lock */
}

static void edgevib_irq_bus_sync_unlock(struct irq_data *data)
{
	/* Virtual device: no hardware bus to sync */
}

/* ---- IRQ domain ops ---- */

static int edgevib_irq_domain_map(struct irq_domain *d, unsigned int virq,
				   irq_hw_number_t hw)
{
	struct edgevib_gpio_priv *priv = d->host_data;

	irq_set_chip_data(virq, priv);
	irq_set_chip(virq, &priv->irq_chip);
	irq_set_noprobe(virq);
	irq_set_nested_thread(virq, 0);

	return 0;
}

static const struct irq_domain_ops edgevib_irq_domain_ops = {
	.map  = edgevib_irq_domain_map,
	.xlate = irq_domain_xlate_twocell, /* gpiolib standard: <gpio offset flags> */
};

/* ---- Custom sysfs attributes ---- */

static ssize_t irq_count_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	if (!g_priv)
		return -ENODEV;
	return sysfs_emit(buf, "%d\n", atomic_read(&g_priv->irq_count));
}

static ssize_t last_irq_time_ms_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	if (!g_priv)
		return -ENODEV;
	return sysfs_emit(buf, "%lld\n",
			  (long long)atomic64_read(&g_priv->last_irq_time_ms));
}

/*
 * inject_irq: simulate a GPIO interrupt from userspace
 *
 * Write: "<line> <value>"  e.g. "4 0" for ESTOP falling edge
 *
 * Uses generic_handle_irq() — the standard public API also used
 * by the kernel's own gpio-mockup test driver.
 */
static ssize_t inject_irq_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct edgevib_gpio_priv *priv = g_priv;
	int line, new_value, virq, irq_offset;
	unsigned long flags;
	int old_value, irq_type;

	if (!priv)
		return -ENODEV;

	if (sscanf(buf, "%d %d", &line, &new_value) != 2)
		return -EINVAL;

	if (line < 4 || line > 5)
		return -EINVAL;

	if (new_value != 0 && new_value != 1)
		return -EINVAL;

	irq_offset = line_to_irq_offset(line);

	spin_lock_irqsave(&priv->lock, flags);
	old_value = priv->line_values[line];
	irq_type = priv->irq_type[irq_offset];

	/* Determine if this edge should trigger based on configured type */
	if ((old_value == 1 && new_value == 0 &&
	     (irq_type & IRQ_TYPE_EDGE_FALLING)) ||
	    (old_value == 0 && new_value == 1 &&
	     (irq_type & IRQ_TYPE_EDGE_RISING)) ||
	    (irq_type & IRQ_TYPE_LEVEL_MASK)) {

		/* Update line value */
		priv->line_values[line] = new_value;

		/* Update stats */
		atomic_inc(&priv->irq_count);
		atomic64_set(&priv->last_irq_time_ms,
			     (long long)jiffies_to_msecs(jiffies));

		spin_unlock_irqrestore(&priv->lock, flags);

		/* Fire the interrupt (must be called outside spinlock) */
		virq = irq_find_mapping(priv->irq_domain, irq_offset);
		if (virq)
			generic_handle_irq(virq);
	} else {
		/* No IRQ triggered — still update the value */
		priv->line_values[line] = new_value;
		spin_unlock_irqrestore(&priv->lock, flags);
	}

	return count;
}

static DEVICE_ATTR_RO(irq_count);
static DEVICE_ATTR_RO(last_irq_time_ms);
static DEVICE_ATTR_WO(inject_irq);

static struct attribute *edgevib_gpio_sysfs_attrs[] = {
	&dev_attr_irq_count.attr,
	&dev_attr_last_irq_time_ms.attr,
	&dev_attr_inject_irq.attr,
	NULL,
};
ATTRIBUTE_GROUPS(edgevib_gpio_sysfs);

/* ---- Module lifecycle ---- */

static int __init edgevib_gpio_init(void)
{
	struct edgevib_gpio_priv *priv;
	int ret;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	spin_lock_init(&priv->lock);
	atomic_set(&priv->irq_count, 0);
	atomic64_set(&priv->last_irq_time_ms, 0);

	/* Initialize default line state */
	priv->line_values[0] = 0;   /* SYSTEM_OK: start low (daemon sets high) */
	priv->line_values[1] = 0;   /* GATEWAY_ALERT: start low */
	priv->line_values[2] = 0;   /* HEARTBEAT: start low */
	priv->line_values[3] = 0;   /* RESERVED: low */
	priv->line_values[4] = 1;   /* ESTOP: pull-up (not pressed) */
	priv->line_values[5] = 0;   /* PSU_FAIL: low (mains OK) */
	priv->line_direction[4] = 0; /* input */
	priv->line_direction[5] = 0; /* input */
	/* lines 0-3 are output (direction 1), will be set by gpioset */

	/* Default IRQ types */
	priv->irq_type[0] = IRQ_TYPE_EDGE_FALLING; /* ESTOP: falling edge */
	priv->irq_type[1] = IRQ_TYPE_EDGE_RISING;  /* PSU_FAIL: rising edge */
	priv->irq_masked[0] = false;
	priv->irq_masked[1] = false;

	/* ---- Register gpio_chip ---- */
	priv->chip.label      = EDGEVIB_GPIO_NAME;
	priv->chip.base       = -1;  /* dynamic allocation */
	priv->chip.ngpio      = EDGEVIB_GPIO_NUM;
	priv->chip.owner      = THIS_MODULE;
	priv->chip.parent     = NULL;
	priv->chip.get_direction   = edgevib_gpio_get_direction;
	priv->chip.direction_input  = edgevib_gpio_direction_input;
	priv->chip.direction_output = edgevib_gpio_direction_output;
	priv->chip.get        = edgevib_gpio_get;
	priv->chip.set        = edgevib_gpio_set;
	priv->chip.to_irq     = edgevib_gpio_to_irq;
	priv->chip.can_sleep  = false;

	ret = gpiochip_add_data(&priv->chip, priv);
	if (ret) {
		pr_err("edgevib_gpio: gpiochip_add_data failed (err=%d)\n", ret);
		goto err_free_priv;
	}

	/* ---- Register IRQ domain ---- */
	priv->irq_chip.name      = EDGEVIB_GPIO_NAME;
	priv->irq_chip.irq_mask  = edgevib_irq_mask;
	priv->irq_chip.irq_unmask = edgevib_irq_unmask;
	priv->irq_chip.irq_set_type = edgevib_irq_set_type;
	priv->irq_chip.irq_bus_lock = edgevib_irq_bus_lock;
	priv->irq_chip.irq_bus_sync_unlock = edgevib_irq_bus_sync_unlock;

	priv->irq_domain = irq_domain_create_linear(
		NULL,                           /* fwnode: NULL for virtual */
		2,                              /* 2 IRQ lines (offset 0,1) */
		&edgevib_irq_domain_ops,
		priv                            /* host_data */
	);
	if (!priv->irq_domain) {
		pr_err("edgevib_gpio: irq_domain_create_linear failed\n");
		ret = -ENOMEM;
		goto err_remove_gpiochip;
	}

	/* ---- Attach sysfs attributes ---- */
	priv->dev = gpiochip_get_dev(&priv->chip);
	if (priv->dev) {
		ret = sysfs_create_groups(&priv->dev->kobj,
					  edgevib_gpio_sysfs_groups);
		if (ret) {
			pr_err("edgevib_gpio: sysfs_create_groups failed (err=%d)\n", ret);
			goto err_remove_irq_domain;
		}
	}

	g_priv = priv;

	pr_info("edgevib_gpio: loaded, chip=%s, base=%d, ngpio=%d, irq_lines=%d\n",
		EDGEVIB_GPIO_NAME, priv->chip.base, EDGEVIB_GPIO_NUM, 2);
	return 0;

err_remove_irq_domain:
	irq_domain_remove(priv->irq_domain);
err_remove_gpiochip:
	gpiochip_remove(&priv->chip);
err_free_priv:
	kfree(priv);
	return ret;
}

static void __exit edgevib_gpio_exit(void)
{
	struct edgevib_gpio_priv *priv = g_priv;

	if (!priv)
		return;

	if (priv->dev)
		sysfs_remove_groups(&priv->dev->kobj, edgevib_gpio_sysfs_groups);
	if (priv->irq_domain)
		irq_domain_remove(priv->irq_domain);
	gpiochip_remove(&priv->chip);
	kfree(priv);
	g_priv = NULL;

	pr_info("edgevib_gpio: unloaded\n");
}

module_init(edgevib_gpio_init);
module_exit(edgevib_gpio_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("EdgeVib Team");
MODULE_DESCRIPTION("EdgeVib Virtual GPIO Controller with IRQ for gateway health monitoring");
