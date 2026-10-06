ifneq ($(KERNELRELEASE),)
obj-m := smit.o
smit-y := smit_usb.o smit_frontend.o smit_transport.o smit_protocol.o smit_worker.o smit_conditional_access.o
ccflags-y += -I$(DVB_USB_V2_DIR)
else
KDIR ?= /lib/modules/$(shell uname -r)/build
DVB_USB_V2_DIR ?=
ARCH ?= x86
CROSS_COMPILE ?=
.PHONY: all clean
all:
	@test -r "$(KDIR)/Module.symvers" || { echo "Missing target Module.symvers"; exit 1; }
	@test -r "$(DVB_USB_V2_DIR)/dvb_usb.h" || { echo "Set DVB_USB_V2_DIR to matching kernel source headers"; exit 1; }
	$(MAKE) -C "$(KDIR)" M="$(CURDIR)" ARCH="$(ARCH)" CROSS_COMPILE="$(CROSS_COMPILE)" DVB_USB_V2_DIR="$(abspath $(DVB_USB_V2_DIR))" modules
clean:
	$(MAKE) -C "$(KDIR)" M="$(CURDIR)" clean
endif
