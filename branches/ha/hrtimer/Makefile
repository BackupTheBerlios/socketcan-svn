ifeq ($(KERNELRELEASE),)

KERNELDIR := /lib/modules/$(shell uname -r)/build
PWD       := $(shell pwd)
TOPDIR    := $(PWD)

export CONFIG_CAN=m
export CONFIG_CAN_RAW=m
export CONFIG_CAN_BCM=m

modules modules_install clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) $@ TOPDIR=$(TOPDIR)

else

obj-m += net/can/

endif
