# EdgeVib Software RTC Driver — Kernel Module + DT Overlay
# Build on Orange Pi: make KERNEL_DIR=/lib/modules/$(uname -r)/build
# Requires: linux-headers-$(uname -r), device-tree-compiler

obj-m := edgevib_rtc.o

KERNEL_DIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

default: edgevib_rtc.ko rtc-edgevib.dtbo

edgevib_rtc.ko:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) modules

rtc-edgevib.dtbo: rtc-edgevib.dts
	dtc -@ -I dts -O dtb -o rtc-edgevib.dtbo rtc-edgevib.dts

clean:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) clean
	rm -f rtc-edgevib.dtbo

install: default
	sudo cp edgevib_rtc.ko /lib/modules/$(shell uname -r)/extra/
	sudo depmod -a
	sudo mkdir -p /boot/overlays
	sudo cp rtc-edgevib.dtbo /boot/overlays/

load: default
	@if [ -d /sys/kernel/config/device-tree/overlays ]; then \
		sudo mkdir -p /sys/kernel/config/device-tree/overlays/rtc-edgevib; \
		sudo sh -c 'cat rtc-edgevib.dtbo > /sys/kernel/config/device-tree/overlays/rtc-edgevib/dtbo' 2>/dev/null; \
	fi
	sudo insmod edgevib_rtc.ko

unload:
	sudo rmmod edgevib_rtc
	@if [ -d /sys/kernel/config/device-tree/overlays/rtc-edgevib ]; then \
		sudo rmdir /sys/kernel/config/device-tree/overlays/rtc-edgevib 2>/dev/null || true; \
	fi

status:
	@lsmod | grep edgevib_rtc || echo "Module not loaded"
	@ls /sys/class/rtc/rtc-* 2>/dev/null || echo "No RTC devices in /sys/class/rtc"
	@cat /proc/driver/rtc 2>/dev/null || true

.PHONY: default clean install load unload status
