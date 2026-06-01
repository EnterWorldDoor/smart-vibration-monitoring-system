# EdgeVib Virtual CAN Kernel Module
# Build on Orange Pi: make KERNEL_DIR=/lib/modules/$(uname -r)/build
# Requires: linux-headers-$(uname -r) installed

obj-m := vcan_edgevib.o

KERNEL_DIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

default:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) clean

install: default
	sudo cp vcan_edgevib.ko /lib/modules/$(shell uname -r)/extra/
	sudo depmod -a
	@echo "Module installed. Use 'sudo modprobe vcan_edgevib' to load."

load:
	sudo insmod vcan_edgevib.ko

unload:
	sudo rmmod vcan_edgevib

status:
	@lsmod | grep vcan_edgevib || echo "Module not loaded"
	@ip link show vcan_edgevib 2>/dev/null || echo "Interface not found"

.PHONY: default clean install load unload status
