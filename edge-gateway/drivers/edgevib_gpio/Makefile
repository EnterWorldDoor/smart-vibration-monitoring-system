# EdgeVib Virtual GPIO Controller — Kbuild
# Build on Orange Pi: make && sudo make install
# Requires: kernel headers at /lib/modules/$(uname -r)/build

obj-m := edgevib_gpio.o

KERNEL_DIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

default:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) clean

install: default
	sudo cp edgevib_gpio.ko /lib/modules/$(shell uname -r)/extra/
	sudo depmod -a
	@echo "Module installed. Use 'sudo modprobe edgevib_gpio' to load."

load:
	sudo insmod edgevib_gpio.ko

unload:
	sudo rmmod edgevib_gpio

status:
	@lsmod | grep edgevib_gpio || echo "Module not loaded"
	@gpiodetect 2>/dev/null | grep edgevib-gpio || echo "  (gpiodetect not available or chip not found)"
	@ls -la /dev/gpiochip* 2>/dev/null || true

.PHONY: default clean install load unload status
