#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/vmalloc.h> /* Возвращен для vmalloc / vfree */
#include <linux/sizes.h>
#include <linux/rwsem.h>

#define MAX_PARTITIONS       3
#define PARTITION_SIZE_MB    100
#define SIMPLE_BLKDEV_MINORS (MAX_PARTITIONS + 1)

static const char *blkdev_name = KBUILD_MODNAME;
static int simple_blkdev_major;

struct simple_blkdev_dev {
    struct gendisk *disk;
    /* Иерархический массив семафоров:
     * locks[0] - глобальный семафор всего диска
     * locks[1..3] - изолированные семафоры разделов */
    struct rw_semaphore locks[SIMPLE_BLKDEV_MINORS];
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

static void simple_blkdev_submit_bio(struct bio *bio)
{
    struct simple_blkdev_dev *dev = bio->bi_bdev->bd_disk->private_data;
    struct bio_vec bvec;
    struct bvec_iter iter;
    bool is_write = (bio_data_dir(bio) == WRITE);

    /* Извлекаем номер раздела: 0 для /dev/simple_blkdev0, 1 для p1, 2 для p2 и т.д. */
    int part_idx = bdev_partno(bio->bi_bdev);

    /* 1. ИЕРАРХИЧЕСКИЙ ЗАХВАТ БЛОКИРОВОК */
    if (part_idx > 0) {
        if (unlikely(part_idx >= SIMPLE_BLKDEV_MINORS))
            part_idx = 0;

        /* Разделы берут глобальный семафор диска в режиме ЧТЕНИЯ (down_read).
         * Все разделы могут работать ПАРАЛЛЕЛЬНО, не блокируя locks[0] друг для друга. */
        down_read(&dev->locks[0]);

        /* Затем раздел захватывает свой персональный семафор */
        if (is_write)
            down_write(&dev->locks[part_idx]);
        else
            down_read(&dev->locks[part_idx]);
    } else {
        /* Запрос к сырому диску (part_idx == 0) захватывает locks[0] в эксклюзивном
         * режиме (Write). Это вежливо останавливает новые запросы ко всем разделам. */
        if (is_write)
            down_write(&dev->locks[0]);
        else
            down_read(&dev->locks[0]);
    }

    /* 2. ЦИКЛ КОПИРОВАНИЯ ДАННЫХ */
    bio_for_each_segment(bvec, bio, iter) {
        loff_t pos = iter.bi_sector * SECTOR_SIZE;
        size_t len = bvec.bv_len;
        void *buf;

        if (unlikely(pos + len > dev->size)) {
            bio->bi_status = BLK_STS_IOERR;
            break;
        }

        /* kmap_local_page мапит строго 4КБ. Благодаря лимитам очереди, len гарантированно <= 4КБ */
        buf = kmap_local_page(bvec.bv_page) + bvec.bv_offset;

        if (is_write)
            memcpy(dev->data + pos, buf, len);
        else
            memcpy(buf, dev->data + pos, len);

        /* Уничтожаем локальный маппинг процессора СТРОГО до освобождения семафоров */
        kunmap_local(buf);
    }

    /* 3. ОСВОБОЖДЕНИЕ БЛОКИРОВОК (В СТРОГО ОБРАТНОМ ПОРЯДКЕ) */
    if (part_idx > 0) {
        if (unlikely(part_idx >= SIMPLE_BLKDEV_MINORS))
            part_idx = 0;

        if (is_write)
            up_write(&dev->locks[part_idx]);
        else
            up_read(&dev->locks[part_idx]);

        /* Освобождаем разделяемый доступ к глобальному диску */
        up_read(&dev->locks[0]);
    } else {
        if (is_write)
            up_write(&dev->locks[0]);
        else
            up_read(&dev->locks[0]);
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
    int p;

    struct queue_limits lim = {
        .logical_block_size = SECTOR_SIZE,
        .physical_block_size = SECTOR_SIZE,
        .io_min = SECTOR_SIZE,
        .max_segment_size = PAGE_SIZE,         /* Один bvec.bv_len не превысит 4096 байт */
        .max_sectors = PAGE_SIZE / SECTOR_SIZE /* Ограничиваем запрос в 8 секторов за раз */
    };

    /* Расчет общего объема диска */
    simple_blkdev.size = (size_t)MAX_PARTITIONS * PARTITION_SIZE_MB * SZ_1M;

    simple_blkdev.data = vmalloc(simple_blkdev.size);
    if (!simple_blkdev.data)
        return -ENOMEM;

    /* Принудительный прогрев страниц для vmalloc */
    memset(simple_blkdev.data, 0, simple_blkdev.size);

    for (p = 0; p < SIMPLE_BLKDEV_MINORS; p++) {
        init_rwsem(&simple_blkdev.locks[p]);
    }

    ret = register_blkdev(0, blkdev_name);
    if (ret < 0) {
        pr_err("register_blkdev %s failed: %d\n", blkdev_name, ret);
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

    snprintf(simple_blkdev.disk->disk_name, sizeof(simple_blkdev.disk->disk_name), "%s%d", blkdev_name, 0);
    set_capacity(simple_blkdev.disk, simple_blkdev.size / SECTOR_SIZE);

    ret = add_disk(simple_blkdev.disk);
    if (ret)
        goto err_cleanup_disk;

    pr_info("module loaded securely. Registered device: /dev/%s0\n", blkdev_name);
    return 0;

err_cleanup_disk:
    put_disk(simple_blkdev.disk);
err_unregister:
    unregister_blkdev(simple_blkdev_major, blkdev_name);
err_vfree:
    vfree(simple_blkdev.data);
    return ret;
}

static void __exit simple_blkdev_exit(void)
{
    del_gendisk(simple_blkdev.disk);
    put_disk(simple_blkdev.disk);
    unregister_blkdev(simple_blkdev_major, blkdev_name);
    vfree(simple_blkdev.data);
    pr_info("module unloaded successfully\n");
}

module_init(simple_blkdev_init);
module_exit(simple_blkdev_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("konygin");
MODULE_DESCRIPTION("RAM block device with vmalloc and interval RW locks");
