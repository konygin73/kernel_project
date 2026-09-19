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
#include <linux/vmalloc.h>

#define SIMPLE_BLKDEV_MINORS            1
#define SIMPLE_BLKDEV_SIZE_MB           300
#define SIMPLE_BLKDEV_NAME              "simple_blkdev"
#define SIMPLE_BLKDEV_FIRST_DISK_INDEX  0

static unsigned int simple_blkdev_size_mb = SIMPLE_BLKDEV_SIZE_MB;
module_param(simple_blkdev_size_mb, uint, 0444);
MODULE_PARM_DESC(simple_blkdev_size_mb, "Размер RAM-диска в МиБ (по умолчанию 300)");

static int simple_blkdev_major;

struct simple_blkdev_dev {
    struct gendisk   *disk;
    void             *data;         /* vmalloc-буфер — весь диск одним блоком */
    size_t            size;         /* размер диска в байтах */
    size_t            num_sectors;  /* размер диска в секторах */
    rwlock_t          lock;
    /* Счётчики для sysfs-статистики */
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

/*
 * Обработчик bio-запросов.
 *
 * Работает в atomic context (прерывание/softirq):
 * - Нельзя спать (schedule, GFP_KERNEL и т.п.)
 * - Нельзя обращаться к памяти, которая может вызвать page fault
 * vmalloc-память безопасна здесь, так как:
 * - Выделена заранее в simple_blkdev_init()
 * - PTE (Page Table Entries) уже установлены
 * - TLB miss приведёт к аппаратному page table walk без сна
 */
static void simple_blkdev_submit_bio(struct bio *bio)
{
    struct simple_blkdev_dev *dev = bio->bi_bdev->bd_disk->private_data;
    struct bio_vec bvec;
    struct bvec_iter iter;
    unsigned long flags;
    unsigned int op = bio_op(bio);
    size_t total_bytes_processed = 0;

    /* 1. Команды без данных — завершаем как успешные */
    if (op == REQ_OP_FLUSH || op == REQ_OP_DISCARD) {
        bio_endio(bio);
        return;
    }

    /* 2. Глобальная проверка границ */
    if (unlikely(bio->bi_iter.bi_sector + bio_sectors(bio) > dev->num_sectors)) {
        bio->bi_status = BLK_STS_IOERR;
        bio_endio(bio);
        pr_err("Out of size\n");
        return;
    }

    /* 3. Обнуление диапазона (mkfs.ext4 активно использует эту команду) */
    if (op == REQ_OP_WRITE_ZEROES) {
        loff_t pos = bio->bi_iter.bi_sector * SECTOR_SIZE;
        size_t len = bio_sectors(bio) * SECTOR_SIZE;

        write_lock_irqsave(&dev->lock, flags);
        memset(dev->data + pos, 0, len);
        write_unlock_irqrestore(&dev->lock, flags);

        bio_endio(bio);
        return;
    }

    /* 4. Обычное чтение/запись */
    bool is_write = (bio_data_dir(bio) == WRITE);

    bio_for_each_segment(bvec, bio, iter) {
        /* kmap_local_page для источника (bvec.bv_page) */
        void *src = kmap_local_page(bvec.bv_page) + bvec.bv_offset;
        size_t len = bvec.bv_len;
        loff_t pos = iter.bi_sector * SECTOR_SIZE;
        void *dst = dev->data + pos;

        if (is_write) {
            write_lock_irqsave(&dev->lock, flags);
            memcpy(dst, src, len);
            write_unlock_irqrestore(&dev->lock, flags);
        } else {
            read_lock_irqsave(&dev->lock, flags);
            memcpy(src, dst, len);
            read_unlock_irqrestore(&dev->lock, flags);
        }

        kunmap_local(src);
        total_bytes_processed += len;
    }

    if (is_write) {
        atomic64_add(total_bytes_processed, &dev->bytes_written);
    } else {
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
/* ИНТЕРФЕЙС SYSFS                                                    */
/* ------------------------------------------------------------------ */

static ssize_t stat_bytes_read_show(struct device *dev,
                                    struct device_attribute *attr, char *buf)
{
    struct gendisk *disk = dev_to_disk(dev);
    struct simple_blkdev_dev *sdev = disk->private_data;
    return sysfs_emit(buf, "%lld\n", (long long)atomic64_read(&sdev->bytes_read));
}

static ssize_t stat_bytes_written_show(struct device *dev,
                                       struct device_attribute *attr, char *buf)
{
    struct gendisk *disk = dev_to_disk(dev);
    struct simple_blkdev_dev *sdev = disk->private_data;
    return sysfs_emit(buf, "%lld\n", (long long)atomic64_read(&sdev->bytes_written));
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

static int __init simple_blkdev_init(void)
{
    int ret;
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

    simple_blkdev.size        = (size_t)simple_blkdev_size_mb * SZ_1M;
    simple_blkdev.num_sectors = simple_blkdev.size / SECTOR_SIZE;

    pr_info("allocating %u MiB via vmalloc\n", simple_blkdev_size_mb);

    simple_blkdev.data = vzalloc(simple_blkdev.size);
    if (!simple_blkdev.data) {
        pr_err("failed to allocate %zu bytes via vmalloc\n", simple_blkdev.size);
        return -ENOMEM;
    }

    rwlock_init(&simple_blkdev.lock);

    ret = register_blkdev(0, SIMPLE_BLKDEV_NAME);
    if (ret < 0) {
        pr_err("register_blkdev failed: %d\n", ret);
        goto err_vfree;
    }
    simple_blkdev_major = ret;

    simple_blkdev.disk = blk_alloc_disk(&lim, NUMA_NO_NODE);
    if (IS_ERR(simple_blkdev.disk)) {
        ret = PTR_ERR(simple_blkdev.disk);
        goto err_unregister;
    }

    simple_blkdev.disk->major        = simple_blkdev_major;
    simple_blkdev.disk->first_minor  = SIMPLE_BLKDEV_FIRST_DISK_INDEX;
    simple_blkdev.disk->minors       = SIMPLE_BLKDEV_MINORS;
    simple_blkdev.disk->fops         = &simple_blkdev_fops;
    simple_blkdev.disk->private_data = &simple_blkdev;

    snprintf(simple_blkdev.disk->disk_name,
             sizeof(simple_blkdev.disk->disk_name),
             "%s%d", SIMPLE_BLKDEV_NAME, SIMPLE_BLKDEV_FIRST_DISK_INDEX);

    set_capacity(simple_blkdev.disk, simple_blkdev.num_sectors);

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
    unregister_blkdev(simple_blkdev_major, SIMPLE_BLKDEV_NAME);
err_vfree:
    vfree(simple_blkdev.data);
    return ret;
}

static void __exit simple_blkdev_exit(void)
{
    sysfs_remove_group(&disk_to_dev(simple_blkdev.disk)->kobj,
                       &simple_blkdev_attr_group);
    /* Исключаем диск из видимости ядра */
    del_gendisk(simple_blkdev.disk);
    /* Освобождаем ресурсы структуры gendisk */
    put_disk(simple_blkdev.disk);
    unregister_blkdev(simple_blkdev_major, SIMPLE_BLKDEV_NAME);
    vfree(simple_blkdev.data);
    pr_info("unloaded\n");
}

module_init(simple_blkdev_init);
module_exit(simple_blkdev_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Konygin");
MODULE_DESCRIPTION("RAM block device 300 MiB via vmalloc with atomic64 stats");
