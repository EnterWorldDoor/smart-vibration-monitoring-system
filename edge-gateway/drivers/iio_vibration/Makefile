# EdgeVib IIO Vibration Kernel Module
# Build on Orange Pi: make KERNEL_DIR=/lib/modules/$(uname -r)/build
# Requires: linux-headers-$(uname -r) installed, CONFIG_IIO=y in kernel

obj-m := edgevib_iio.o

KERNEL_DIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

default:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) clean

install: default
	sudo cp edgevib_iio.ko /lib/modules/$(shell uname -r)/extra/
	sudo depmod -a
	@echo "Module installed. Use 'sudo modprobe edgevib_iio' to load."

load:
	sudo insmod edgevib_iio.ko

unload:
	sudo rmmod edgevib_iio

status:
	@lsmod | grep edgevib_iio || echo "Module not loaded"
	@ls -la /dev/edgevib-iio-inject 2>/dev/null || echo "Device node not found"
	@ls /sys/bus/iio/devices/ 2>/dev/null || echo "No IIO devices"

.PHONY: default clean install load unload status
