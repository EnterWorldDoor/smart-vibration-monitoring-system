/*
 * edgevib_minimal.c — Minimal block device module to test blk-mq
 * Compile: copy to .. && obj-m rename
 * Test: insmod -> lsblk -> dd if=/dev/zero of=/dev/edgevib-buffer bs=4096 count=1 -> rmmod
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/delay.h>

#define DEV_NAME "edgevib-buffer"

static struct gendisk *g_disk;
static struct blk_mq_tag_set g_tag_set;
static u8 *g_ring;
static int g_sectors = 256;

static blk_status_t my_queue_rq(struct blk_mq_hw_ctx *hctx,
                                const struct blk_mq_queue_data *bd)
{
    struct request *rq = bd->rq;
    struct bio_vec bv;
    struct req_iterator iter;
    unsigned long flags;
    u32 sector;
    static DEFINE_SPINLOCK(lock);

    blk_mq_start_request(rq);
    sector = blk_rq_pos(rq);

    spin_lock_irqsave(&lock, flags);

    rq_for_each_segment(bv, rq, iter) {
        void *page = kmap_local_page(bv.bv_page);
        u32 idx = (sector % g_sectors) * 4096;

        if (req_op(rq) == REQ_OP_WRITE && g_ring)
            memcpy(g_ring + idx, page + bv.bv_offset, bv.bv_len);
        else if (req_op(rq) == REQ_OP_READ && g_ring)
            memcpy(page + bv.bv_offset, g_ring + idx, bv.bv_len);

        kunmap_local(page);
        sector++;
    }

    spin_unlock_irqrestore(&lock, flags);
    blk_mq_end_request(rq, BLK_STS_OK);
    return BLK_STS_OK;
}

static const struct blk_mq_ops my_ops = {
    .queue_rq = my_queue_rq,
};

static int __init my_init(void)
{
    int ret;

    pr_info("minimal_blk: allocating ring (%d sectors)\n", g_sectors);
    g_ring = vzalloc(g_sectors * 4096);
    if (!g_ring) return -ENOMEM;

    memset(&g_tag_set, 0, sizeof(g_tag_set));
    g_tag_set.ops = &my_ops;
    g_tag_set.nr_hw_queues = 1;
    g_tag_set.queue_depth = 8;
    g_tag_set.numa_node = NUMA_NO_NODE;
    g_tag_set.flags = BLK_MQ_F_SHOULD_MERGE;

    ret = blk_mq_alloc_tag_set(&g_tag_set);
    if (ret) {
        pr_err("minimal_blk: tag_set alloc failed %d\n", ret);
        vfree(g_ring);
        return ret;
    }

    g_disk = blk_mq_alloc_disk(&g_tag_set, NULL);
    if (IS_ERR(g_disk)) {
        pr_err("minimal_blk: alloc_disk failed\n");
        blk_mq_free_tag_set(&g_tag_set);
        vfree(g_ring);
        return PTR_ERR(g_disk);
    }

    blk_queue_logical_block_size(g_disk->queue, 512);
    blk_queue_physical_block_size(g_disk->queue, 512);
    blk_queue_max_hw_sectors(g_disk->queue, 8);
    g_disk->flags |= GENHD_FL_NO_PART;  /* prevent partition scan during add_disk */
    snprintf(g_disk->disk_name, DISK_NAME_LEN, DEV_NAME);
    set_capacity(g_disk, g_sectors * 8);  /* 8x more sectors for 512-byte blocks */

    pr_info("minimal_blk: tag_set+disk allocated OK\n");
    pr_info("minimal_blk: sysfs steps done, about to add_disk...\n");
    ssleep(3);
    pr_info("minimal_blk: calling device_add_disk NOW\n");
    ret = device_add_disk(NULL, g_disk, NULL);
    if (ret) {
        pr_err("minimal_blk: device_add_disk failed %d\n", ret);
        put_disk(g_disk);
        blk_mq_free_tag_set(&g_tag_set);
        vfree(g_ring);
        return ret;
    }

    pr_info("minimal_blk: /dev/%s loaded (%d x 4KB sectors)\n", DEV_NAME, g_sectors);
    return 0;
}

static void __exit my_exit(void)
{
    if (g_disk) {
        del_gendisk(g_disk);
        put_disk(g_disk);
    }
    blk_mq_free_tag_set(&g_tag_set);
    vfree(g_ring);
    pr_info("minimal_blk: unloaded\n");
}

module_init(my_init);
module_exit(my_exit);
MODULE_LICENSE("GPL");
