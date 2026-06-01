/*
 * vcan_edgevib.c — EdgeVib Virtual CAN Network Driver
 *
 * Creates a virtual CAN interface (vcan_edgevib) that accepts CAN frames
 * injected from userspace via AF_CAN socket writes and echoes them into
 * the receive path so they're visible to candump/cansniffer/Wireshark.
 *
 * Usage:
 *   insmod vcan_edgevib.ko
 *   ip link set vcan_edgevib up
 *   candump vcan_edgevib &        # See frames
 *   cansend vcan_edgevib 201#AABBCCDDEEFF  # Test injection
 *
 * The Go daemon (edgevib-can-d) uses socket(AF_CAN, SOCK_RAW, CAN_RAW)
 * → bind → write(can_frame) to inject real NDE sensor CAN frames.
 *
 * SPDX-License-Identifier: GPL-2.0
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/netdevice.h>
#include <linux/can.h>
#include <linux/can/dev.h>
#include <linux/can/skb.h>
#include <linux/sysfs.h>
#include <linux/atomic.h>
#include <linux/slab.h>

#define VCAN_EDGEVIB_NAME  "vcan_edgevib"

static struct net_device *vcan_dev;

/* Custom sysfs counters */
static atomic_t crc_errors    = ATOMIC_INIT(0);
static atomic_t fifo_overruns = ATOMIC_INIT(0);

/* ---- sysfs attributes ---- */

static ssize_t crc_errors_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", atomic_read(&crc_errors));
}

static ssize_t fifo_overruns_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", atomic_read(&fifo_overruns));
}

static DEVICE_ATTR_RO(crc_errors);
static DEVICE_ATTR_RO(fifo_overruns);

static struct attribute *vcan_attrs[] = {
	&dev_attr_crc_errors.attr,
	&dev_attr_fifo_overruns.attr,
	NULL,
};
ATTRIBUTE_GROUPS(vcan);

/* Public accessors for Go daemon (exposed via sysfs) */
/* The Go daemon reads these via /sys/class/net/vcan_edgevib/device/crc_errors etc. */

/* ---- net_device_ops ---- */

static int vcan_open(struct net_device *dev)
{
	netif_start_queue(dev);
	return 0;
}

static int vcan_stop(struct net_device *dev)
{
	netif_stop_queue(dev);
	return 0;
}

/*
 * vcan_start_xmit — Primary injection point
 *
 * Called by the kernel CAN stack when userspace writes a CAN frame
 * (struct can_frame) to an AF_CAN socket bound to vcan_edgevib.
 *
 * We echo the frame back into the receive path via netif_rx(),
 * so tools like candump can see it.
 */
static netdev_tx_t vcan_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct canfd_frame *cf = (struct canfd_frame *)skb->data;
	struct sk_buff *rx_skb;
	struct can_frame *rx_cf;

	/* Allocate a fresh CAN skb for the RX path */
	rx_skb = alloc_can_skb(dev, &rx_cf);
	if (!rx_skb) {
		atomic_inc(&fifo_overruns);
		dev->stats.tx_dropped++;
		dev_kfree_skb(skb);
		return NETDEV_TX_OK;
	}

	/* Copy CAN frame contents */
	rx_cf->can_id = cf->can_id & CAN_EFF_MASK;
	rx_cf->len    = cf->len;
	memcpy(rx_cf->data, cf->data, sizeof(cf->data));
	rx_skb->ip_summed = CHECKSUM_UNNECESSARY;

	/* Inject into receive path — now visible in candump */
	netif_rx(rx_skb);

	dev->stats.tx_packets++;
	dev->stats.tx_bytes += cf->len;
	dev_kfree_skb(skb);

	return NETDEV_TX_OK;
}

static const struct net_device_ops vcan_netdev_ops = {
	.ndo_open        = vcan_open,
	.ndo_stop        = vcan_stop,
	.ndo_start_xmit  = vcan_start_xmit,
};

/* ---- Module lifecycle ---- */

static int __init vcan_edgevib_init(void)
{
	int ret;

	vcan_dev = alloc_candev(0, 0);
	if (!vcan_dev)
		return -ENOMEM;

	vcan_dev->netdev_ops = &vcan_netdev_ops;
	vcan_dev->flags &= ~IFF_ECHO;  /* Don't echo TX back to sender's socket */
	vcan_dev->dev.groups = vcan_groups;

	/* Assign fixed interface name */
	strscpy(vcan_dev->name, VCAN_EDGEVIB_NAME, IFNAMSIZ);

	ret = register_candev(vcan_dev);
	if (ret) {
		free_candev(vcan_dev);
		pr_err("vcan_edgevib: register_candev failed (err=%d)\n", ret);
		return ret;
	}

	pr_info("vcan_edgevib: loaded, interface=%s\n", vcan_dev->name);
	return 0;
}

static void __exit vcan_edgevib_exit(void)
{
	if (vcan_dev) {
		unregister_candev(vcan_dev);
		free_candev(vcan_dev);
	}
	pr_info("vcan_edgevib: unloaded\n");
}

module_init(vcan_edgevib_init);
module_exit(vcan_edgevib_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("EdgeVib Team");
MODULE_DESCRIPTION("EdgeVib Virtual CAN interface for NDE sensor data diagnostics");
