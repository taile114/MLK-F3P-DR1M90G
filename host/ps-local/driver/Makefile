ifneq ($(KERNELRELEASE),)
obj-m := pl_ctrl.o
else
KDIR ?= /lib/modules/$(shell uname -r)/build
ARCH ?= arm64

.PHONY: all clean
all:
	$(MAKE) -C "$(KDIR)" M="$(CURDIR)" ARCH="$(ARCH)" CROSS_COMPILE="$(CROSS_COMPILE)" modules
clean:
	$(MAKE) -C "$(KDIR)" M="$(CURDIR)" clean
endif
