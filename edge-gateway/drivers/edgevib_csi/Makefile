obj-m := edgevib_csi_sensor.o

KERNEL_DIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

default: edgevib_csi_sensor.ko edgevib-csi-ov5640.dtbo

edgevib_csi_sensor.ko:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) modules

edgevib-csi-ov5640.dtbo: edgevib-csi-ov5640.dts
	dtc -@ -I dts -O dtb -o edgevib-csi-ov5640.dtbo edgevib-csi-ov5640.dts

clean:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) clean
	rm -f edgevib-csi-ov5640.dtbo

install: default
	sudo cp edgevib_csi_sensor.ko /lib/modules/$(shell uname -r)/extra/
	sudo depmod -a
	sudo mkdir -p /boot/overlays
	sudo cp edgevib-csi-ov5640.dtbo /boot/overlays/
	@echo "Module and DT overlay installed. Hardware required for testing."

load:
	@echo "D7b requires physical OV5640 MIPI CSI camera hardware."
	@echo "Manual steps:"
	@echo "  1. Insert OV5640 FPC cable into Orange Pi MIPI CSI connector"
	@echo "  2. sudo dtoverlay edgevib-csi-ov5640.dtbo"
	@echo "  3. sudo modprobe edgevib_csi_sensor"
	@echo "  4. media-ctl -p | grep ov5640"

unload:
	sudo rmmod edgevib_csi_sensor

.PHONY: default clean install load unload
