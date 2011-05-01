

#
#  $Id: Makefile 1127 2010-02-18 18:37:20Z hartkopp $
#

ifeq ($(KERNELRELEASE),)

KERNELDIR := /lib/modules/$(shell uname -r)/build
PWD       := $(shell pwd)
TOPDIR    := $(PWD)/../..

modules modules_install clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) $@ TOPDIR=$(TOPDIR)

export CONFIG_CAN=m
export CONFIG_CAN_RAW=m
export CONFIG_CAN_BCM=m
export CONFIG_CAN_ISOTP=m
export CONFIG_CAN_GW=m
export CONFIG_CAN_J1939=m

else

-include $(TOPDIR)/Makefile.common

obj-$(CONFIG_CAN_J1939)	+= can-j1939.o

can-j1939-objs := main.o \
	proc.o bus.o \
	rtnl.o \
	socket.o \
	address-claim.o transport.o \
	promisc.o filter.o

ifeq ($(CONFIG_CAN_J1939_DEBUG),y)
	EXTRA_CFLAGS += -DDEBUG
endif

endif