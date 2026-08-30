PWD := $(shell pwd)
KERNEL_DIR ?= /lib/modules/$(shell uname -r)/build

DRV_NAME := simple_blkdev

#CLANG_FORMAT_VERS ?= 14
#CLANG_FORMAT := clang-format-$(CLANG_FORMAT_VERS)
CLANG_FORMAT_FLAGS += -i
#FORMAT_FILES := $(SRC_DIR)/*.c

.PHONY: build run remove install uninstall clean format

build:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) modules

format:
	@echo "Форматирование исходного кода с помощью clang-format..."
	clang-format $(CLANG_FORMAT_FLAGS) src/*.c *.h 2>/dev/null || true

run:
	insmod $(PWD)/$(DRV_NAME).ko

remove:
	rmmod $(DRV_NAME)

install:
	cp $(DRV_NAME).ko /lib/modules/$(shell uname -r)
	depmod -a
	modprobe $(DRV_NAME)

uninstall:
	modprobe -r $(DRV_NAME)
	rm /lib/modules/$(shell uname -r)/$(DRV_NAME).ko
	depmod -a

clean:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) clean

check:
	@echo "Запуск проверочного скрипта check.sh..."
	./check.sh
