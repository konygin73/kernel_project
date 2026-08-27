#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/vmalloc.h>
#include <linux/spinlock.h>
#include <linux/sizes.h>

/* 1 minor для самого диска + 3 minor для разделов = 4 */
#define SIMPLE_BLKDEV_MINORS 4 
#define PARTITION_COUNT      3
#define PARTITION_SIZE_MB    100

static int simple_blkdev_major;

struct simple_blkdev_dev {
    struct gendisk *disk;
    spinlock_t lock;
    u8 *data;       /* Единый массив для всего диска (300 МиБ) */
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

static void simple_blkdev_submit_bio(struct bio *bio)
{
    struct simple_blkdev_dev *dev = bio->bi_bdev->bd_disk->private_data;
    struct bio_vec bvec;
    struct bvec_iter iter;

    bio_for_each_segment(bvec, bio, iter) {
        /* Вычисляем позицию строго по итератору текущего сегмента */
        loff_t pos = iter.bi_sector * SECTOR_SIZE;
        size_t len = bvec.bv_len;
        void *buf;

        /* Безопасность: проверка границ всего диска */
        if (unlikely(pos + len > dev->size)) {
            bio->bi_status = BLK_STS_IOERR;
            break;
        }

        buf = kmap_local_page(bvec.bv_page) + bvec.bv_offset;

        /* Благодаря прогреву vmalloc в init, здесь НЕ будет Page Fault */
        spin_lock(&dev->lock);
        if (bio_data_dir(bio) == WRITE)
            memcpy(dev->data + pos, buf, len);
        else
            memcpy(buf, dev->data + pos, len);
        spin_unlock(&dev->lock);

        kunmap_local(buf);
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
    size_t i;

    /* Общий размер устройства: 3 раздела * 100 МиБ = 300 МиБ */
    simple_blkdev.size = (size_t)PARTITION_COUNT * PARTITION_SIZE_MB * SZ_1M;
    
    simple_blkdev.data = vmalloc(simple_blkdev.size);
    if (!simple_blkdev.data)
        return -ENOMEM;

    /* КРИТИЧЕСКИ ВАЖНО: Прогреваем память vmalloc, чтобы синхронизировать */
    /* таблицы страниц ядра сейчас, а не внутри submit_bio под спин-локом! */
    for (i = 0; i < simple_blkdev.size; i += PAGE_SIZE) {
        volatile u8 *ptr = (volatile u8 *)(simple_blkdev.data + i);
        *ptr = 0;
    }

    spin_lock_init(&simple_blkdev.lock);

    ret = register_blkdev(0, "simple_blkdev");
    if (ret < 0) {
        pr_err("simple_blkdev: register_blkdev failed: %d\n", ret);
        goto err_vfree;
    }
    simple_blkdev_major = ret;

    simple_blkdev.disk = blk_alloc_disk(NUMA_NO_NODE);
    if (IS_ERR(simple_blkdev.disk)) {
        ret = PTR_ERR(simple_blkdev.disk);
        goto err_unregister;
    }

    simple_blkdev.disk->major = simple_blkdev_major;
    simple_blkdev.disk->first_minor = 0;
    
    /* Задаем количество миноров (диск + разделы) */
    simple_blkdev.disk->minors = SIMPLE_BLKDEV_MINORS; 
    
    /* ВАЖНО: Удален флаг GENHD_FL_NO_PART, чтобы ядро разрешило разделы */
    simple_blkdev.disk->fops = &simple_blkdev_fops;
    simple_blkdev.disk->private_data = &simple_blkdev;
    
    sprintf(simple_blkdev.disk->disk_name, "simple_blkdev%d", 0);
    set_capacity(simple_blkdev.disk, simple_blkdev.size / SECTOR_SIZE);

    ret = add_disk(simple_blkdev.disk);
    if (ret)
        goto err_cleanup_disk;

    pr_info("simple_blkdev: registered, total capacity %llu sectors\n", 
            get_capacity(simple_blkdev.disk));
    return 0;

err_cleanup_disk:
    put_disk(simple_blkdev.disk);
err_unregister:
    unregister_blkdev(simple_blkdev_major, "simple_blkdev");
err_vfree:
    vfree(simple_blkdev.data);
    return ret;
}

static void __exit simple_blkdev_exit(void)
{
    del_gendisk(simple_blkdev.disk);
    put_disk(simple_blkdev.disk);
    unregister_blkdev(simple_blkdev_major, "simple_blkdev");
    vfree(simple_blkdev.data);
    pr_info("simple_blkdev: unloaded\n");
}

module_init(simple_blkdev_init);
module_exit(simple_blkdev_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("otus-lession demo");
MODULE_DESCRIPTION("RAM-backed block device with partition support");
