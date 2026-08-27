#!/bin/bash

# Проверяем, запущен ли скрипт от root
if [ "$EUID" -ne 0 ]; then
  echo "❌ Пожалуйста, запустите скрипт с правами sudo"
  exit 1
fi

# Загрузка модуля: kmem_cache, таймер каждые 2 секунды
insmod kernel_msgpool.ko alloc_type=0 interval_ms=2000

# Отправка нескольких сообщений
echo "message one"   > /sys/module/kernel_msgpool/parameters/send
echo "message two"   > /sys/module/kernel_msgpool/parameters/send
echo "message three" > /sys/module/kernel_msgpool/parameters/send

# Через 2 секунды таймер сработает — в dmesg:
# msgpool: message one   (queued 2001345000 ns ago)
# msgpool: [^1] message two   (queued 2001100000 ns ago)
# msgpool: [^2] message three (queued 2000800000 ns ago)

# Прочитать последнее обработанное сообщение
cat /sys/module/kernel_msgpool/parameters/inbox
# message three

# Статистика
cat /sys/module/kernel_msgpool/parameters/stats
# sent=3 consumed=3 flushed=0 dropped=0 queued=0 alloc=kmem_cache interval_ms=2000

sleep 3

cat /sys/module/kernel_msgpool/parameters/inbox

cat /sys/module/kernel_msgpool/parameters/stats

# Переключиться на mempool
echo 1 > /sys/module/kernel_msgpool/parameters/alloc_type

echo "mempool message" > /sys/module/kernel_msgpool/parameters/send

# Принудительный сброс (освободить не обработанные сообщения)
echo 1 > /sys/module/kernel_msgpool/parameters/flush

# Изменить период таймера
echo 500 > /sys/module/kernel_msgpool/parameters/interval_ms

# Выгрузка
sudo rmmod kernel_msgpool
