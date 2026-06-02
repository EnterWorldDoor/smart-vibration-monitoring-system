# EdgeVib RAM Buffer Block Device Kernel Module
# Build on Orange Pi: make KERNEL_DIR=/lib/modules/$(uname -r)/build

obj-m := edgevib_buffer.o

KERNEL_DIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

default:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) clean

install: default
	sudo cp edgevib_buffer.ko /lib/modules/$(shell uname -r)/extra/
	sudo depmod -a
	@echo "Module installed. Use 'sudo modprobe edgevib_buffer' to load."

load:
	sudo insmod edgevib_buffer.ko ring_size_mb=1

unload:
	sudo rmmod edgevib_buffer

status:
	@lsmod | grep edgevib_buffer || echo "Module not loaded"
	@lsblk | grep edgevib-buffer || echo "Block device not found"

.PHONY: default clean install load unload status
