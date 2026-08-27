#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/vmalloc.h> /* Возвращен для vmalloc / vfree */
#include <linux/sizes.h>
#include <linux/rwsem.h> 

#define PARTITION_COUNT      3
#define PARTITION_SIZE_MB    100
#define PARTITION_SIZE_BYTES ((size_t)PARTITION_SIZE_MB * SZ_1M)
#define SIMPLE_BLKDEV_MINORS (PARTITION_COUNT + 1) 

static int simple_blkdev_major;

struct simple_blkdev_dev {
    struct gendisk *disk;
    struct rw_semaphore locks[PARTITION_COUNT];
    u8 *data;       /* Единый массив для всего диска на vmalloc (300 МиБ) */
    size_t size;    /* Полный размер в байтах */
};

static struct simple_blkdev_dev simple_blkdev;

static int simple_blkdev_open(struct gendisk *disk, blk_mode_t mode)
{
    return 0;
}

static void simple_blkdev_release(struct gendisk *disk)
{
}

static inline int get_interval_index(loff_t pos)
{
    int index = (int)(pos / PARTITION_SIZE_BYTES);
    if (unlikely(index >= PARTITION_COUNT))
        index = PARTITION_COUNT - 1;
    return index;
}

static void simple_blkdev_submit_bio(struct bio *bio)
{
    struct simple_blkdev_dev *dev = bio->bi_bdev->bd_disk->private_data;
    struct bio_vec bvec;
    struct bvec_iter iter;
    bool is_write = (bio_data_dir(bio) == WRITE);

    bio_for_each_segment(bvec, bio, iter) {
        loff_t pos = iter.bi_sector * SECTOR_SIZE;
        size_t len = bvec.bv_len;
        void *buf;
        int interval_idx;

        if (unlikely(pos + len > dev->size)) {
            bio->bi_status = BLK_STS_IOERR;
            break;
        }

        interval_idx = get_interval_index(pos);

        if (is_write)
            down_write(&dev->locks[interval_idx]);
        else
            down_read(&dev->locks[interval_idx]);

        buf = kmap_local_page(bvec.bv_page) + bvec.bv_offset;

        if (is_write)
            memcpy(dev->data + pos, buf, len);
        else
            memcpy(buf, dev->data + pos, len);

        kunmap_local(buf);

        if (is_write)
            up_write(&dev->locks[interval_idx]);
        else
            up_read(&dev->locks[interval_idx]);
    }

    bio_endio(bio);
}

static const struct block_device_operations simple_blkdev_fops = {
    .owner = THIS_MODULE,
    .submit_bio = simple_blkdev_submit_bio,
    .open = simple_blkdev_open,
    .release = simple_blkdev_release,
};

static int __init simple_blkdev_init(void)
{
    int ret;
    size_t i; /* Переменная корректно объявлена */
    int p;

    struct queue_limits lim = {
        .logical_block_size = SECTOR_SIZE,
        .physical_block_size = SECTOR_SIZE,
        .io_min = SECTOR_SIZE,
    };

    simple_blkdev.size = (size_t)PARTITION_COUNT * PARTITION_SIZE_BYTES;
    
    /* ВОЗВРАЩЕНО: Выделение виртуально непрерывной памяти ядра */
    simple_blkdev.data = vmalloc(simple_blkdev.size);
    if (!simple_blkdev.data)
        return -ENOMEM;

    /* ОБЯЗАТЕЛЬНО: Принудительный прогрев страниц для vmalloc */
    for (i = 0; i < simple_blkdev.size; i += PAGE_SIZE) {
        volatile u8 *ptr = (volatile u8 *)(simple_blkdev.data + i);
        *ptr = 0;
    }

    for (p = 0; p < PARTITION_COUNT; p++) {
        init_rwsem(&simple_blkdev.locks[p]);
    }

    ret = register_blkdev(0, "simple_blkdev");
    if (ret < 0) {
        pr_err("simple_blkdev: register_blkdev failed: %d\n", ret);
        goto err_vfree;
    }
    simple_blkdev_major = ret;

    simple_blkdev.disk = blk_alloc_disk(&lim, NUMA_NO_NODE);
    if (IS_ERR(simple_blkdev.disk)) {
        ret = PTR_ERR(simple_blkdev.disk);
        goto err_unregister;
    }

    simple_blkdev.disk->major = simple_blkdev_major;
    simple_blkdev.disk->first_minor = 0;
    simple_blkdev.disk->minors = SIMPLE_BLKDEV_MINORS; 
    simple_blkdev.disk->fops = &simple_blkdev_fops;
    simple_blkdev.disk->private_data = &simple_blkdev;
    
    sprintf(simple_blkdev.disk->disk_name, "simple_blkdev%d", 0);
    set_capacity(simple_blkdev.disk, simple_blkdev.size / SECTOR_SIZE);

    ret = add_disk(simple_blkdev.disk);
    if (ret)
        goto err_cleanup_disk;

    pr_info("simple_blkdev: registered with vmalloc and interval rw-semaphores\n");
    return 0;

err_cleanup_disk:
    put_disk(simple_blkdev.disk);
err_unregister:
    unregister_blkdev(simple_blkdev_major, "simple_blkdev");
err_vfree:
    vfree(simple_blkdev.data); /* Заменено на корректный vfree */
    return ret;
}

static void __exit simple_blkdev_exit(void)
{
    del_gendisk(simple_blkdev.disk);
    put_disk(simple_blkdev.disk);
    unregister_blkdev(simple_blkdev_major, "simple_blkdev");
    vfree(simple_blkdev.data); /* Заменено на корректный vfree */
    pr_info("simple_blkdev: unloaded\n");
}

module_init(simple_blkdev_init);
module_exit(simple_blkdev_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("otus-lession demo");
MODULE_DESCRIPTION("RAM block device with vmalloc and interval RW locks");
