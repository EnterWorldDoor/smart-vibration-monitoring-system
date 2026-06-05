obj-m := edgevib_video.o

KERNEL_DIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

default:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) clean

install: default
	sudo cp edgevib_video.ko /lib/modules/$(shell uname -r)/extra/
	sudo depmod -a
	@echo "Module installed. Use 'sudo modprobe edgevib_video num_devices=2' to load."

load:
	sudo insmod edgevib_video.ko num_devices=2

unload:
	sudo rmmod edgevib_video

status:
	@lsmod | grep edgevib_video || echo "Module not loaded"
	@for d in /dev/video*; do \
		v4l2-ctl -d $$d --info 2>/dev/null | grep -q "edgevib_video" && echo "$$d: edgevib_video"; \
	done || true
	@cat /sys/devices/virtual/edgevib-video-inject/frame_count 2>/dev/null || echo "  sysfs not available"

.PHONY: default clean install load unload status
