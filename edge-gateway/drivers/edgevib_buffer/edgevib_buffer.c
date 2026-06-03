/*
 * edgevib_buffer.c — EdgeVib RAM Buffer Block Device (blk-mq)
 *
 * Registers /dev/edgevib-buffer — a 1MB RAM-based ring buffer block
 * device for IoT data staging. Uses blk-mq for request handling.
 *
 * Architecture:
 *   flush-d (Go daemon):
 *     MQTT subscribe → write(/dev/edgevib-buffer)  [Producer]
 *     read(/dev/edgevib-buffer) → batch INSERT → TimescaleDB  [Consumer]
 *
 *   Multiple consumers can open the block device and read independently.
 *   Writes wrap around the ring buffer (oldest data overwritten).
 *
 * Sector layout (4096 bytes each):
 *   [0-3]:   magic         0x56424544 ("EDVB")
 *   [4-7]:   crc32         CRC32 of payload bytes
 *   [8-15]:  timestamp_ms  Unix-epoch milliseconds (producer set)
 *   [16-19]: payload_len   JSONB payload length (0-4072)
 *   [20-23]: device_count  Devices in this batch
 *   [24-27]: batch_seq     Monotonic batch sequence number
 *   [28-31]: reserved
 *   [32-..]: payload       JSONB array (up to 4064 bytes)
 *
 * sysfs (standard block):
 *   /sys/block/edgevib-buffer/stat
 *
 * sysfs (custom):
 *   /sys/devices/virtual/block/edgevib-buffer/overrun_sectors
 *   /sys/devices/virtual/block/edgevib-buffer/oldest_sector_age_ms
 *
 * Usage:
 *   insmod edgevib_buffer.ko ring_size_mb=2
 *   lsblk /dev/edgevib-buffer
 *   dd if=/dev/zero of=/dev/edgevib-buffer bs=4096 count=1
 *   dd if=/dev/edgevib-buffer bs=4096 count=1 | hexdump -C
 *
 * SPDX-License-Identifier: GPL-2.0
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/bio.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include <linux/atomic.h>
#include <linux/jiffies.h>
#include <linux/crc32.h>
#include <linux/sysfs.h>

#define EDGEVIB_BUF_NAME  "edgevib-buffer"
#define EDGEVIB_SECTOR_SIZE  4096
#define EDGEVIB_HEADER_SIZE  32
#define EDGEVIB_MAX_PAYLOAD  4064  /* 4096 - 32 header */
#define EDGEVIB_DEFAULT_RING_MB  1

/* ---- Module parameter ---- */

static int ring_size_mb = EDGEVIB_DEFAULT_RING_MB;
module_param(ring_size_mb, int, 0444);
MODULE_PARM_DESC(ring_size_mb, "Ring buffer size in MB (default 1, max 128)");

/* ---- Device state ---- */

struct edgevib_buf_dev {
	struct gendisk          *disk;
	struct blk_mq_tag_set    tag_set;
	struct request_queue     *queue;

	/* Ring buffer */
	u8    *ring_buffer;         /* kmalloc'd ring */
	size_t ring_bytes;          /* total ring size */
	u32    total_sectors;       /* ring_bytes / 4096 */
	u32    write_head;          /* next write sector index */

	/* Custom sysfs counters */
	atomic_t   overrun_sectors;   /* sectors overwritten */
	atomic64_t oldest_timestamp; /* oldest valid sector's timestamp_ms */

	/* Lock for ring buffer access */
	spinlock_t lock;
};

static struct edgevib_buf_dev *g_bdev;

/* ---- Custom sysfs attributes ---- */

static ssize_t overrun_sectors_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	if (!g_bdev)
		return -ENODEV;
	return sysfs_emit(buf, "%d\n", atomic_read(&g_bdev->overrun_sectors));
}

static ssize_t oldest_sector_age_ms_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	u64 ts, now_ms;

	if (!g_bdev)
		return -ENODEV;

	ts = atomic64_read(&g_bdev->oldest_timestamp);
	now_ms = jiffies_to_msecs(jiffies);

	if (ts == 0 || ts > now_ms)
		return sysfs_emit(buf, "0\n");

	return sysfs_emit(buf, "%llu\n", now_ms - ts);
}

static DEVICE_ATTR_RO(overrun_sectors);
static DEVICE_ATTR_RO(oldest_sector_age_ms);

static struct attribute *edgevib_buffer_attrs[] = {
	&dev_attr_overrun_sectors.attr,
	&dev_attr_oldest_sector_age_ms.attr,
	NULL,
};
ATTRIBUTE_GROUPS(edgevib_buffer);

/* ---- Stats ---- */

static void ring_write_u32(u8 *sector, u32 offset, u32 val)
{
	sector[offset]   = val & 0xFF;
	sector[offset+1] = (val >> 8) & 0xFF;
	sector[offset+2] = (val >> 16) & 0xFF;
	sector[offset+3] = (val >> 24) & 0xFF;
}

/* ---- blk-mq: single queue_rq callback ----
 *
 * Handles both READ and WRITE bio requests.
 * READ: copies from ring buffer to bio pages
 * WRITE: copies from bio pages to ring buffer, updates headers
 */

static blk_status_t edgevib_queue_rq(struct blk_mq_hw_ctx *hctx,
				     const struct blk_mq_queue_data *bd)
{
	struct request *rq = bd->rq;
	struct edgevib_buf_dev *bdev = hctx->queue->queuedata;
	struct bio_vec bv;
	struct req_iterator iter;
	sector_t sector;
	u32 sector_idx;
	unsigned long flags;

	if (!bdev || !bdev->ring_buffer) {
		blk_mq_end_request(rq, BLK_STS_IOERR);
		return BLK_STS_IOERR;
	}

	blk_mq_start_request(rq);

	spin_lock_irqsave(&bdev->lock, flags);

	sector = blk_rq_pos(rq);

	rq_for_each_segment(bv, rq, iter) {
		void *page_ptr;
		unsigned int seg_len = bv.bv_len;
		unsigned int seg_off = bv.bv_offset;
		sector_t cur_sector = sector + (seg_off / EDGEVIB_SECTOR_SIZE);
		u32 seg_offset_in_sector = seg_off % EDGEVIB_SECTOR_SIZE;

		page_ptr = kmap_local_page(bv.bv_page);

		if (req_op(rq) == REQ_OP_WRITE) {
			/* Write: page → ring buffer */
			void *from = page_ptr + seg_off;
			unsigned int bytes_left = seg_len;

			while (bytes_left > 0) {
				sector_idx = cur_sector % bdev->total_sectors;
				u32 sector_base = sector_idx * EDGEVIB_SECTOR_SIZE;
				u32 offset = seg_offset_in_sector;
				u32 chunk = min_t(u32, bytes_left,
						  EDGEVIB_SECTOR_SIZE - offset);

				/* Detect overrun: write cursor caught up to a possibly unread sector */
				if (cur_sector - bdev->write_head >= bdev->total_sectors)
					atomic_inc(&bdev->overrun_sectors);

				memcpy(bdev->ring_buffer + sector_base + offset,
				       from, chunk);

				/* Stamp header on first byte of new sector */
				if (offset == 0 && chunk >= EDGEVIB_HEADER_SIZE) {
					ring_write_u32(bdev->ring_buffer + sector_base,
						       0, 0x56424544); /* magic "EDVB" */

					/* timestamp_ms */
					{
						u64 ts = jiffies_to_msecs(jiffies);
						u8 *p = bdev->ring_buffer + sector_base + 8;
						p[0] = ts & 0xFF;
						p[1] = (ts >> 8) & 0xFF;
						p[2] = (ts >> 16) & 0xFF;
						p[3] = (ts >> 24) & 0xFF;
						p[4] = (ts >> 32) & 0xFF;
						p[5] = (ts >> 40) & 0xFF;
						p[6] = (ts >> 48) & 0xFF;
						p[7] = (ts >> 56) & 0xFF;
					}
				}

				bytes_left -= chunk;
				from += chunk;
				cur_sector++;
				seg_offset_in_sector = 0;
			}

			bdev->write_head = sector_idx + 1;

		} else if (req_op(rq) == REQ_OP_READ) {
			/* Read: ring buffer → page */
			void *to = page_ptr + seg_off;
			unsigned int bytes_left = seg_len;

			while (bytes_left > 0) {
				sector_idx = cur_sector % bdev->total_sectors;
				u32 sector_base = sector_idx * EDGEVIB_SECTOR_SIZE;
				u32 offset = seg_offset_in_sector;
				u32 chunk = min_t(u32, bytes_left,
						  EDGEVIB_SECTOR_SIZE - offset);

				memcpy(to,
				       bdev->ring_buffer + sector_base + offset,
				       chunk);

				bytes_left -= chunk;
				to += chunk;
				cur_sector++;
				seg_offset_in_sector = 0;
			}
		}

		kunmap_local(page_ptr);
	}

	spin_unlock_irqrestore(&bdev->lock, flags);

	blk_mq_end_request(rq, BLK_STS_OK);
	return BLK_STS_OK;
}

static const struct blk_mq_ops edgevib_mq_ops = {
	.queue_rq = edgevib_queue_rq,
};

/* ---- Module lifecycle ---- */

static int __init edgevib_buffer_init(void)
{
	struct edgevib_buf_dev *bdev;
	int ret;

	/* Validate ring size */
	if (ring_size_mb < 1 || ring_size_mb > 128) {
		pr_err("edgevib_buffer: ring_size_mb must be 1-128 (got %d)\n",
		       ring_size_mb);
		return -EINVAL;
	}

	/* Allocate device struct */
	bdev = kzalloc(sizeof(*bdev), GFP_KERNEL);
	if (!bdev)
		return -ENOMEM;

	spin_lock_init(&bdev->lock);
	atomic_set(&bdev->overrun_sectors, 0);
	atomic64_set(&bdev->oldest_timestamp, 0);

	/* Calculate dimensions */
	bdev->ring_bytes = (size_t)ring_size_mb * 1024 * 1024;
	bdev->total_sectors = bdev->ring_bytes / EDGEVIB_SECTOR_SIZE;

	/* Allocate ring buffer */
	bdev->ring_buffer = vzalloc(bdev->ring_bytes);
	if (!bdev->ring_buffer) {
		pr_err("edgevib_buffer: failed to allocate %zu MiB ring buffer\n",
		       (size_t)ring_size_mb);
		ret = -ENOMEM;
		goto err_free_dev;
	}

	/* Set up blk-mq tag set */
	memset(&bdev->tag_set, 0, sizeof(bdev->tag_set));
	bdev->tag_set.ops          = &edgevib_mq_ops;
	bdev->tag_set.nr_hw_queues = 1;
	bdev->tag_set.queue_depth  = 8;
	bdev->tag_set.numa_node    = NUMA_NO_NODE;
	bdev->tag_set.flags        = BLK_MQ_F_SHOULD_MERGE;
	bdev->tag_set.cmd_size     = 0;

	ret = blk_mq_alloc_tag_set(&bdev->tag_set);
	if (ret) {
		pr_err("edgevib_buffer: blk_mq_alloc_tag_set failed (err=%d)\n", ret);
		goto err_free_ring;
	}

	/* Allocate gendisk + request_queue (blk-mq modern API) */
	bdev->disk = blk_mq_alloc_disk(&bdev->tag_set, bdev);
	if (IS_ERR(bdev->disk)) {
		ret = PTR_ERR(bdev->disk);
		pr_err("edgevib_buffer: blk_mq_alloc_disk failed (err=%d)\n", ret);
		goto err_free_tag_set;
	}

	bdev->queue = bdev->disk->queue;

	/* Configure block device */
	blk_queue_logical_block_size(bdev->queue, EDGEVIB_SECTOR_SIZE);
	blk_queue_physical_block_size(bdev->queue, EDGEVIB_SECTOR_SIZE);
	blk_queue_max_hw_sectors(bdev->queue, 8);  /* max 8×4KB = 32KB */
	blk_queue_flag_set(QUEUE_FLAG_NONROT, bdev->queue);    /* RAM */
	blk_queue_flag_set(QUEUE_FLAG_NOMERGES, bdev->queue);  /* ring semantic */

	/* Configure gendisk */
	bdev->disk->major       = 0;  /* dynamic allocation */
	bdev->disk->first_minor = 0;
	bdev->disk->minors      = 1;
	bdev->disk->fops        = NULL;  /* block layer handles open/close */
	snprintf(bdev->disk->disk_name, DISK_NAME_LEN, EDGEVIB_BUF_NAME);
	set_capacity(bdev->disk, bdev->total_sectors);

	/* Attach custom sysfs attributes */
	disk_to_dev(bdev->disk)->groups = edgevib_buffer_groups;

	/* Register the block device */
	ret = device_add_disk(NULL, bdev->disk, NULL);
	if (ret) {
		pr_err("edgevib_buffer: device_add_disk failed (err=%d)\n", ret);
		goto err_put_disk;
	}

	g_bdev = bdev;

	pr_info("edgevib_buffer: loaded, dev=/dev/%s, sectors=%u, size=%zuMiB, queue_depth=8\n",
		EDGEVIB_BUF_NAME, bdev->total_sectors, (size_t)ring_size_mb);
	return 0;

err_put_disk:
	put_disk(bdev->disk);
err_free_tag_set:
	blk_mq_free_tag_set(&bdev->tag_set);
err_free_ring:
	vfree(bdev->ring_buffer);
err_free_dev:
	kfree(bdev);
	return ret;
}

static void __exit edgevib_buffer_exit(void)
{
	struct edgevib_buf_dev *bdev = g_bdev;

	if (!bdev)
		return;

	del_gendisk(bdev->disk);
	put_disk(bdev->disk);
	blk_mq_free_tag_set(&bdev->tag_set);
	vfree(bdev->ring_buffer);
	kfree(bdev);
	g_bdev = NULL;

	pr_info("edgevib_buffer: unloaded\n");
}

module_init(edgevib_buffer_init);
module_exit(edgevib_buffer_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("EdgeVib Team");
MODULE_DESCRIPTION("EdgeVib RAM Buffer Block Device for IoT data staging");
