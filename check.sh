#!/bin/bash

if [ "$EUID" -ne 0 ]; then
  echo "❌ Пожалуйста, запустите скрипт с правами sudo"
  exit 1
fi

echo "🔄 Загрузка модуля..."
insmod simple_blkdev.ko || { echo "❌ Ошибка загрузки модуля"; exit 1; }

echo "🔄 Разметка диска (GPT, 3 раздела)..."
parted -s /dev/simple_blkdev0 mklabel gpt
parted -s /dev/simple_blkdev0 mkpart primary ext4 1MiB 100MiB
parted -s /dev/simple_blkdev0 mkpart primary ext4 100MiB 200MiB
parted -s /dev/simple_blkdev0 mkpart primary ext4 200MiB 100%

echo "🔄 Перечитывание таблицы разделов..."
blockdev --rereadpt /dev/simple_blkdev0
sleep 1 # Небольшая пауза для гарантии применения изменений ядром

echo "🔄 Форматирование разделов..."
mkfs.ext4 -F /dev/simple_blkdev0p1
mkfs.ext4 -F /dev/simple_blkdev0p2
mkfs.ext4 -F /dev/simple_blkdev0p3

echo "🔄 Монтирование..."
mkdir -p /mnt/ram_p1 /mnt/ram_p2 /mnt/ram_p3
mount /dev/simple_blkdev0p1 /mnt/ram_p1
mount /dev/simple_blkdev0p2 /mnt/ram_p2
mount /dev/simple_blkdev0p3 /mnt/ram_p3

echo "📊 Структура разделов:"
lsblk | grep simple_blkdev

echo "📊 Статистика ДО записи:"
cat /sys/block/simple_blkdev0/stat_bytes_read
cat /sys/block/simple_blkdev0/stat_bytes_written

echo "📝 Копирование тестового файла и синхронизация..."
# Создаем тестовый файл, если его нет
if [ ! -f ./check.sh ]; then
    echo "echo 'test'" > ./check.sh
    chmod +x ./check.sh
fi
cp ./check.sh /mnt/ram_p1/
sync # Принудительная запись всех кэшей на диск (вызовет REQ_OP_FLUSH)

echo "📊 Статистика ПОСЛЕ записи:"
cat /sys/block/simple_blkdev0/stat_bytes_read
cat /sys/block/simple_blkdev0/stat_bytes_written

echo "🔄 Очистка и выгрузка..."
# 1. Отмонтирование
umount /mnt/ram_p1 /mnt/ram_p2 /mnt/ram_p3

# 2. Принудительный сброс буферов блочного устройства (критично перед удалением!)
blockdev --flushbufs /dev/simple_blkdev0

# 3. Удаление таблиц разделов из ядра (игнорируем ошибки, если уже удалены)
partx -d /dev/simple_blkdev0 2>/dev/null || true

# 4. Выгрузка модуля
rmmod simple_blkdev

echo "✅ Готово. Последние сообщения ядра:"
dmesg | tail -n 15
