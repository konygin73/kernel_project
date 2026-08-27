# kernel_project
Драйвер блочного устройства с тремя разделами

sudo insmod simple_blkdev.ko
sudo parted -s /dev/simple_blkdev0 mklabel gpt
sudo parted -s /dev/simple_blkdev0 mkpart primary ext4 1MiB 101MiB
sudo parted -s /dev/simple_blkdev0 mkpart primary ext4 101MiB 201MiB
sudo parted -s /dev/simple_blkdev0 mkpart primary ext4 201MiB 100%

lsblk