# kernel_project
# Драйвер блочного устройства с максимум тремя разделами
# Конфигурационный файл ядра: config-7.0.3

# 1. Компиляция модуля
make

# 2. Тестовый скрипт check.sh

# 3. Загрузка в ядро
sudo insmod simple_blkdev.ko
sudo dmesg | tail -n 5
# simple_blkdev: module loaded securely. Registered device: /dev/simple_blkdev0

# 4. Разметка диска на 3 раздела средствами parted
sudo parted -s /dev/simple_blkdev0 mklabel gpt
sudo parted -s /dev/simple_blkdev0 mkpart primary ext4 1MiB 100MiB
sudo parted -s /dev/simple_blkdev0 mkpart primary ext4 100MiB 200MiB
sudo parted -s /dev/simple_blkdev0 mkpart primary ext4 200MiB 100%

# 5. Перечитывание таблицы разделов блочного устройства
sudo blockdev --rereadpt /dev/simple_blkdev0

# 6. Форматирование разделов
sudo mkfs.ext4 /dev/simple_blkdev0p1
sudo mkfs.ext4 /dev/simple_blkdev0p2
sudo mkfs.ext4 /dev/simple_blkdev0p3

# 7. Монтирование
sudo mkdir -p /mnt/ram_p1 /mnt/ram_p2 /mnt/ram_p3
sudo mount /dev/simple_blkdev0p1 /mnt/ram_p1
sudo mount /dev/simple_blkdev0p2 /mnt/ram_p2
sudo mount /dev/simple_blkdev0p3 /mnt/ram_p3

# 8. Проверка созданной структуры разделов
lsblk | grep simple_blkdev

# Чтение статистики
# FILE *fp = fopen("/sys/block/simple_blkdev0/stat_bytes_written", "r"); fscanf(fp, "%llu", &bytes);
# наблюдение каждую секунду: watch -n 1 cat /sys/block/simple_blkdev0/stat_bytes_written
cat /sys/block/simple_blkdev0/stat_bytes_read
cat /sys/block/simple_blkdev0/stat_bytes_written

# тестовое копирование произвольного файла
cp ./check.sh /mnt/ram_p1
# отправить данные из системного кэша страниц вниз, в драйвер
sync

# Удаления (выгрузки) блочного драйвера
# 1. Отмонтирование файловых систем
sudo umount /mnt/ram_p1
sudo umount /mnt/ram_p2
sudo umount /mnt/ram_p3

# Закрытие удерживающих процессов
sudo fuser -mvk /mnt/ram_p1 /mnt/ram_p2 /mnt/ram_p3

# 2. Очистка таблиц разделов ядра
sudo partx -d /dev/simple_blkdev0

# 3. Выгрузка модуля драйвера из ядра
sudo rmmod simple_blkdev

sudo dmesg | tail -n 10
# simple_blkdev: module unloaded successfully
