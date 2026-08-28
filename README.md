# kernel_project
# Драйвер блочного устройства с максимум тремя разделами

# 1. Компиляция модуля
make

# 2. Загрузка в ядро
sudo insmod kernel_ram_dsk.ko
sudo dmesg | tail -n 5
# kernel_ram_dsk: module loaded securely. Registered device: /dev/kernel_ram_dsk0

# 4. Разметка диска на 3 раздела средствами parted
sudo parted -s /dev/kernel_ram_dsk0 mklabel gpt
sudo parted -s /dev/kernel_ram_dsk0 mkpart primary ext4 1MiB 100MiB
sudo parted -s /dev/kernel_ram_dsk0 mkpart primary ext4 100MiB 200MiB
sudo parted -s /dev/kernel_ram_dsk0 mkpart primary ext4 200MiB 100%

# 5. Перечитывание таблицы разделов блочного устройства
sudo blockdev --rereadpt /dev/kernel_ram_dsk0

# 6. Форматирование разделов
sudo mkfs.ext4 /dev/kernel_ram_dsk0p1
sudo mkfs.ext4 /dev/kernel_ram_dsk0p2
sudo mkfs.ext4 /dev/kernel_ram_dsk0p3

# 7. Монтирование
sudo mkdir -p /mnt/ram_p1 /mnt/ram_p2 /mnt/ram_p3
sudo mount /dev/kernel_ram_dsk0p1 /mnt/ram_p1
sudo mount /dev/kernel_ram_dsk0p2 /mnt/ram_p2
sudo mount /dev/kernel_ram_dsk0p3 /mnt/ram_p3

# 8. Проверка созданной структуры разделов
lsblk | grep kernel_ram_dsk


# Удаления (выгрузки) блочного драйвера
# 1. Отмонтирование файловых систем
sudo umount /mnt/ram_p1
sudo umount /mnt/ram_p2
sudo umount /mnt/ram_p3

# Закрытие удерживающих процессов
sudo fuser -mvk /mnt/ram_p1 /mnt/ram_p2 /mnt/ram_p3

# 2. Очистка таблиц разделов ядра
sudo partx -d /dev/kernel_ram_dsk0

# 3. Выгрузка модуля драйвера из ядра
sudo rmmod kernel_ram_dsk

sudo dmesg | tail -n 5
# kernel_ram_dsk: module unloaded successfully
