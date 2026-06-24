KDIR ?= /lib/modules/$(shell uname -r)/build
DTC ?= dtc
PWD := $(CURDIR)

default: modules

modules:
	$(MAKE) -C $(KDIR) M="$(PWD)" modules

flir-lepton-rpi5.dtbo: flir-lepton-rpi5.dts
	$(DTC) -@ -I dts -O dtb -o $@ $<

overlay: flir-lepton-rpi5.dtbo

clean:
	rm -f *.o *.a *.ko *.mod *.mod.c *.dtbo Module.symvers modules.order .*.cmd

.PHONY: default modules overlay clean
