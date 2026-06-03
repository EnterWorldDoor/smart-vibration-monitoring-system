obj-m := edgevib_hwmon.o

KERNEL_DIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

default:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) clean

install: default
	sudo cp edgevib_hwmon.ko /lib/modules/$(shell uname -r)/extra/
	sudo depmod -a
	@echo "Module installed. Use 'sudo modprobe edgevib_hwmon num_motors=4' to load."

load:
	sudo insmod edgevib_hwmon.ko num_motors=4

unload:
	sudo rmmod edgevib_hwmon

.PHONY: default clean install load unload
