#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/slab.h>
#include <linux/sizes.h>
#include <linux/spinlock.h>
#include <linux/sysfs.h>
#include <linux/atomic.h>

#define SIMPLE_BLKDEV_MINORS      1
#define SIMPLE_BLKDEV_SIZE_MB     300
#define PAGES_PER_CHUNK           512  /* 512 × 8 = PAGE_SIZE */

static unsigned int simple_blkdev_size_mb = SIMPLE_BLKDEV_SIZE_MB;
module_param(simple_blkdev_size_mb, uint, 0444);
MODULE_PARM_DESC(simple_blkdev_size_mb, "Размер RAM-диска в МиБ (по умолчанию 300)");

static int simple_blkdev_major;

struct simple_blkdev_dev {
    struct gendisk   *disk;
    struct page    ***chunks;//[150][512][4096]
    size_t            num_chunks;
    size_t            num_pages;
    size_t            num_sectors;
    size_t            size;
    rwlock_t          lock;

    /* счетчики для sysfs статистики */
    atomic64_t        bytes_read;
    atomic64_t        bytes_written;
};

static struct simple_blkdev_dev simple_blkdev;

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
    unsigned int op = bio_op(bio);
    size_t total_bytes_processed = 0;

    if (op == REQ_OP_FLUSH || op == REQ_OP_DISCARD) {
        bio_endio(bio);
        return;
    }

    if (unlikely(bio->bi_iter.bi_sector + bio_sectors(bio) > dev->num_sectors)) {
        bio->bi_status = BLK_STS_IOERR;
        bio_endio(bio);
        pr_err("Out of size\n");
        return;
    }

    if (op == REQ_OP_WRITE_ZEROES) {
        sector_t sector = bio->bi_iter.bi_sector;
        unsigned int nr_sectors = bio_sectors(bio);
        loff_t pos = sector * SECTOR_SIZE;
        size_t len = nr_sectors * SECTOR_SIZE;
        size_t offset = 0;

        write_lock_irqsave(&dev->lock, flags);
        while (offset < len) {
            pgoff_t page_idx = (pos + offset) / PAGE_SIZE;
            size_t page_off  = (pos + offset) % PAGE_SIZE;
            size_t chunk_len = min_t(size_t, len - offset, PAGE_SIZE - page_off);

            size_t chunk_n = page_idx / PAGES_PER_CHUNK;
            size_t page_n = page_idx % PAGES_PER_CHUNK;

            struct page *c_page = dev->chunks[chunk_n][page_n];
            void *dst = kmap_local_page(c_page) + page_off;

            memset(dst, 0, chunk_len);

            kunmap_local(dst);
            offset += chunk_len;
        }
        write_unlock_irqrestore(&dev->lock, flags);

        bio_endio(bio);
        return;
    }

    bool is_write = (bio_data_dir(bio) == WRITE);

    if (is_write)
        write_lock_irqsave(&dev->lock, flags);
    else
        read_lock_irqsave(&dev->lock, flags);

    bio_for_each_segment(bvec, bio, iter) {
        void *bvec_src = kmap_local_page(bvec.bv_page) + bvec.bv_offset;
        size_t need_len = bvec.bv_len;
        size_t completed = 0;

        /* Вычисляем глобальную позицию в байтах для текущего сегмента */
        loff_t pos = iter.bi_sector * SECTOR_SIZE;

        while (completed < need_len) {
            /* Пересчитываем индексы на каждом шагу с учетом смещения completed */
            pgoff_t page_idx = (pos + completed) / PAGE_SIZE;
            size_t page_off  = (pos + completed) % PAGE_SIZE;

            /* Защита от выхода за границы внутри сегмента */
            if (unlikely(page_idx >= dev->num_pages)) {
                bio->bi_status = BLK_STS_IOERR;
                break;
            }

            size_t chunk_n = page_idx / PAGES_PER_CHUNK;
            size_t page_n = page_idx % PAGES_PER_CHUNK;

            /* Вычисляем безопасную длину: не больше остатка сегмента и не дальше конца текущей страницы */
            size_t chunk_len = min_t(size_t, need_len - completed, PAGE_SIZE - page_off);
            
            struct page *c_page = dev->chunks[chunk_n][page_n];
            void *dst = kmap_local_page(c_page) + page_off;
        
            if (is_write)
                memcpy(dst, bvec_src + completed, chunk_len);
            else
                memcpy(bvec_src + completed, dst, chunk_len);

            kunmap_local(dst);

            completed += chunk_len;
            total_bytes_processed += chunk_len;
        }
        kunmap_local(bvec_src);
    }

    if (is_write) {
        write_unlock_irqrestore(&dev->lock, flags);
        atomic64_add(total_bytes_processed, &dev->bytes_written);
    } else {
        read_unlock_irqrestore(&dev->lock, flags);
        atomic64_add(total_bytes_processed, &dev->bytes_read);
    }

    bio_endio(bio);
}

static const struct block_device_operations simple_blkdev_fops = {
    .owner      = THIS_MODULE,
    .submit_bio = simple_blkdev_submit_bio,
    .open       = simple_blkdev_open,
    .release    = simple_blkdev_release,
};

/* ------------------------------------------------------------------ */
/* РЕАЛИЗАЦИЯ ИНТЕРФЕЙСА SYSFS                                        */
/* ------------------------------------------------------------------ */

static ssize_t stat_bytes_read_show(struct device *dev,
                                    struct device_attribute *attr, char *buf)
{
    struct gendisk *disk = dev_to_disk(dev);
    struct simple_blkdev_dev *sdev = disk->private_data;
    return sysfs_emit(buf, "%lld\n", atomic64_read(&sdev->bytes_read));
}

static ssize_t stat_bytes_written_show(struct device *dev,
                                       struct device_attribute *attr, char *buf)
{
    struct gendisk *disk = dev_to_disk(dev);
    struct simple_blkdev_dev *sdev = disk->private_data;
    return sysfs_emit(buf, "%lld\n", atomic64_read(&sdev->bytes_written));
}

static DEVICE_ATTR_RO(stat_bytes_read);
static DEVICE_ATTR_RO(stat_bytes_written);

static struct attribute *simple_blkdev_attrs[] = {
    &dev_attr_stat_bytes_read.attr,
    &dev_attr_stat_bytes_written.attr,
    NULL,
};

static const struct attribute_group simple_blkdev_attr_group = {
    .attrs = simple_blkdev_attrs,
};

/* ------------------------------------------------------------------ */

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

    atomic64_set(&simple_blkdev.bytes_read, 0);
    atomic64_set(&simple_blkdev.bytes_written, 0);

    simple_blkdev.size       = (size_t)simple_blkdev_size_mb * SZ_1M;
    simple_blkdev.num_pages  = simple_blkdev.size / PAGE_SIZE;
    simple_blkdev.num_chunks = DIV_ROUND_UP(simple_blkdev.num_pages, PAGES_PER_CHUNK);
    simple_blkdev.num_sectors = simple_blkdev.size / SECTOR_SIZE;

    pr_info("allocating static %u MiB: %zu pages in %zu chunks\n",
            simple_blkdev_size_mb,
            simple_blkdev.num_pages,
            simple_blkdev.num_chunks);

    simple_blkdev.chunks = kcalloc(simple_blkdev.num_chunks,
                                   sizeof(struct page **),
                                   GFP_KERNEL);
    if (!simple_blkdev.chunks) {
        pr_err("failed to allocate chunks array\n");
        return -ENOMEM;
    }

    for (i = 0; i < simple_blkdev.num_chunks; i++) {
        size_t pages_in_chunk = min_t(size_t, PAGES_PER_CHUNK, simple_blkdev.num_pages - i * PAGES_PER_CHUNK);

        simple_blkdev.chunks[i] = kcalloc(pages_in_chunk,
                                          sizeof(struct page *),
                                          GFP_KERNEL);
        if (!simple_blkdev.chunks[i]) {
            ret = -ENOMEM;
            pr_err("failed to allocate chunk[%zu]\n", i);
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

    ret = sysfs_create_group(&disk_to_dev(simple_blkdev.disk)->kobj,
                             &simple_blkdev_attr_group);
    if (ret) {
        pr_err("failed to create sysfs group\n");
        goto err_del_disk;
    }

    pr_info("registered /dev/simple_blkdev0: %u MiB\n", simple_blkdev_size_mb);
    return 0;

err_del_disk:
    del_gendisk(simple_blkdev.disk);
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
    sysfs_remove_group(&disk_to_dev(simple_blkdev.disk)->kobj,
                       &simple_blkdev_attr_group);
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
MODULE_DESCRIPTION("RAM block device 300 MiB with atomic64 stats and without reclaim");
