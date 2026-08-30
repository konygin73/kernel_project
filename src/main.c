#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/slab.h>
#include <linux/sizes.h>
#include <linux/spinlock.h>

#define SIMPLE_BLKDEV_MINORS      1
#define SIMPLE_BLKDEV_SIZE_MB     300
#define PAGES_PER_CHUNK           512  /* 512 × 8 = PAGE_SIZE */

static unsigned int simple_blkdev_size_mb = SIMPLE_BLKDEV_SIZE_MB;
module_param(simple_blkdev_size_mb, uint, 0444);
MODULE_PARM_DESC(simple_blkdev_size_mb, "Размер RAM-диска в МиБ (по умолчанию 300)");

static int simple_blkdev_major;

struct simple_blkdev_dev {
    struct gendisk   *disk;
    struct page    ***chunks;
    size_t            num_chunks;
    size_t            num_pages;
    size_t            size;
    rwlock_t          lock;
};

static struct simple_blkdev_dev simple_blkdev;

static inline struct page *dev_get_page(struct simple_blkdev_dev *dev,
                                        pgoff_t idx)
{
    return dev->chunks[idx / PAGES_PER_CHUNK][idx % PAGES_PER_CHUNK];
}

static int simple_blkdev_open(struct gendisk *disk, blk_mode_t mode)
{
    return 0;
}

static void simple_blkdev_release(struct gendisk *disk)
{
}

static void simple_blkdev_submit_bio(struct bio *bio)
{
    struct simple_blkdev_dev *dev = bio->bi_bdev->bd_disk->private_data;
    struct bio_vec bvec;
    struct bvec_iter iter;
    unsigned long flags;
    bool is_write = (bio_data_dir(bio) == WRITE);

    if (bio_op(bio) == REQ_OP_FLUSH || bio_op(bio) == REQ_OP_DISCARD) {
        bio_endio(bio);
        return;
    }

    if (is_write)
        write_lock_irqsave(&dev->lock, flags);
    else
        read_lock_irqsave(&dev->lock, flags);

    bio_for_each_segment(bvec, bio, iter) {
        void *src = kmap_local_page(bvec.bv_page) + bvec.bv_offset;
        size_t len = bvec.bv_len;
        loff_t pos = iter.bi_sector * SECTOR_SIZE;
        size_t offset = 0;

        if (unlikely(pos + len > dev->size)) {
            bio->bi_status = BLK_STS_IOERR;
            kunmap_local(src);
            break;
        }

        while (offset < len) {
            pgoff_t page_idx = (pos + offset) / PAGE_SIZE;
            size_t page_off  = (pos + offset) % PAGE_SIZE;
            size_t chunk_len = min_t(size_t, len - offset,
                                     PAGE_SIZE - page_off);

            void *dst = kmap_local_page(dev_get_page(dev, page_idx))
                        + page_off;

            if (is_write)
                memcpy(dst, src + offset, chunk_len);
            else
                memcpy(src + offset, dst, chunk_len);

            kunmap_local(dst);
            offset += chunk_len;
        }

        kunmap_local(src);
    }

    if (is_write)
        write_unlock_irqrestore(&dev->lock, flags);
    else
        read_unlock_irqrestore(&dev->lock, flags);

    bio_endio(bio);
}

static const struct block_device_operations simple_blkdev_fops = {
    .owner      = THIS_MODULE,
    .submit_bio = simple_blkdev_submit_bio,
    .open       = simple_blkdev_open,
    .release    = simple_blkdev_release,
};

static void simple_blkdev_free_all(void)
{
    size_t i, j;

    if (!simple_blkdev.chunks)
        return;

    for (i = 0; i < simple_blkdev.num_chunks; i++) {
        if (!simple_blkdev.chunks[i])
            continue;

        size_t pages_in_chunk = min_t(size_t, PAGES_PER_CHUNK,
                                      simple_blkdev.num_pages - i * PAGES_PER_CHUNK);
        for (j = 0; j < pages_in_chunk; j++) {
            if (simple_blkdev.chunks[i][j])
                __free_page(simple_blkdev.chunks[i][j]);
        }
        kfree(simple_blkdev.chunks[i]);
    }
    kfree(simple_blkdev.chunks);
    simple_blkdev.chunks = NULL;
}

static int __init simple_blkdev_init(void)
{
    int ret;
    size_t i, j;
    struct queue_limits lim = {
        .logical_block_size  = SECTOR_SIZE,
        .physical_block_size = SECTOR_SIZE,
        .max_segment_size    = PAGE_SIZE,
        .max_sectors         = BLK_SAFE_MAX_SECTORS,
    };

    if (!simple_blkdev_size_mb) {
        pr_err("simple_blkdev_size_mb must be non-zero\n");
        return -EINVAL;
    }

    simple_blkdev.size       = (size_t)simple_blkdev_size_mb * SZ_1M;
    simple_blkdev.num_pages  = simple_blkdev.size / PAGE_SIZE;
    simple_blkdev.num_chunks = DIV_ROUND_UP(simple_blkdev.num_pages,
                                            PAGES_PER_CHUNK);

    pr_info("allocating %u MiB: %zu pages in %zu chunks\n",
            simple_blkdev_size_mb,
            simple_blkdev.num_pages,
            simple_blkdev.num_chunks);

    /* Массив chunks: 150 × 8 = 1200 байт */
    simple_blkdev.chunks = kcalloc(simple_blkdev.num_chunks,
                                   sizeof(struct page **),
                                   GFP_KERNEL);
    if (!simple_blkdev.chunks) {
        pr_err("failed to allocate chunks array\n");
        return -ENOMEM;
    }

    /* Каждый chunk: 512 × 8 = 4096 байт = PAGE_SIZE */
    for (i = 0; i < simple_blkdev.num_chunks; i++) {
        size_t pages_in_chunk = min_t(size_t, PAGES_PER_CHUNK,
                                      simple_blkdev.num_pages - i * PAGES_PER_CHUNK);

        simple_blkdev.chunks[i] = kcalloc(pages_in_chunk,
                                          sizeof(struct page *),
                                          GFP_KERNEL);
        if (!simple_blkdev.chunks[i]) {
            pr_err("failed to allocate chunk[%zu]\n", i);
            ret = -ENOMEM;
            goto err_free;
        }

        for (j = 0; j < pages_in_chunk; j++) {
            simple_blkdev.chunks[i][j] = alloc_page(GFP_KERNEL | __GFP_ZERO);
            if (!simple_blkdev.chunks[i][j]) {
                pr_err("failed to allocate page[%zu][%zu]\n", i, j);
                ret = -ENOMEM;
                goto err_free;
            }
        }
    }

    rwlock_init(&simple_blkdev.lock);

    ret = register_blkdev(0, "simple_blkdev");
    if (ret < 0) {
        pr_err("register_blkdev failed: %d\n", ret);
        goto err_free;
    }
    simple_blkdev_major = ret;

    simple_blkdev.disk = blk_alloc_disk(&lim, NUMA_NO_NODE);
    if (IS_ERR(simple_blkdev.disk)) {
        ret = PTR_ERR(simple_blkdev.disk);
        goto err_unregister;
    }

    simple_blkdev.disk->major        = simple_blkdev_major;
    simple_blkdev.disk->first_minor  = 0;
    simple_blkdev.disk->minors       = SIMPLE_BLKDEV_MINORS;
    simple_blkdev.disk->fops         = &simple_blkdev_fops;
    simple_blkdev.disk->private_data = &simple_blkdev;

    snprintf(simple_blkdev.disk->disk_name,
             sizeof(simple_blkdev.disk->disk_name),
             "simple_blkdev%d", 0);

    set_capacity(simple_blkdev.disk, simple_blkdev.size / SECTOR_SIZE);

    ret = add_disk(simple_blkdev.disk);
    if (ret) {
        pr_err("add_disk failed: %d\n", ret);
        goto err_cleanup_disk;
    }

    pr_info("registered /dev/simple_blkdev0: %u MiB, %llu sectors, %zu pages, %zu chunks\n",
            simple_blkdev_size_mb,
            (unsigned long long)get_capacity(simple_blkdev.disk),
            simple_blkdev.num_pages,
            simple_blkdev.num_chunks);
    return 0;

err_cleanup_disk:
    put_disk(simple_blkdev.disk);
err_unregister:
    unregister_blkdev(simple_blkdev_major, "simple_blkdev");
err_free:
    simple_blkdev_free_all();
    return ret;
}

static void __exit simple_blkdev_exit(void)
{
    del_gendisk(simple_blkdev.disk);
    put_disk(simple_blkdev.disk);
    unregister_blkdev(simple_blkdev_major, "simple_blkdev");

    simple_blkdev_free_all();

    pr_info("unloaded\n");
}

module_init(simple_blkdev_init);
module_exit(simple_blkdev_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("konygin");
MODULE_DESCRIPTION("RAM block device 300 MiB with chunked per-page allocation");
