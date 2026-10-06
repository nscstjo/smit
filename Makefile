# SMIT iCast USB-C/DTMB USB Tuner. Run GNU Make on a Linux build host.
.DEFAULT_GOAL := all
PLATFORM ?= ubuntu-x86_64
OUT ?= $(CURDIR)/build/$(PLATFORM)
KDIR ?= /lib/modules/$(shell uname -r)/build
DVB_USB_V2_DIR ?=
ARCH ?= x86
CROSS_COMPILE ?=
ifeq ($(origin CC),default)
CC := $(CROSS_COMPILE)gcc
endif
SDK ?=
RESOURCE_CACHE ?=
JOBS ?= 4
SUPPORTED_PLATFORMS := ubuntu-x86_64 openwrt-mt7621
ifeq ($(filter $(PLATFORM),$(SUPPORTED_PLATFORMS)),)
$(error Unsupported PLATFORM '$(PLATFORM)'; choose $(SUPPORTED_PLATFORMS))
endif
.PHONY: all ubuntu openwrt module server package check help clean
ifeq ($(PLATFORM),ubuntu-x86_64)
all: ubuntu
module:
	@mkdir -p "$(OUT)/kernel/driver" "$(OUT)/kernel/include"
	cp -u driver/*.[ch] driver/Makefile "$(OUT)/kernel/driver/"
	cp -u include/*.h "$(OUT)/kernel/include/"
	$(MAKE) -C "$(OUT)/kernel/driver" KDIR="$(KDIR)" DVB_USB_V2_DIR="$(DVB_USB_V2_DIR)" ARCH="$(ARCH)" CROSS_COMPILE="$(CROSS_COMPILE)" CC="$(CC)" W=1
	cp "$(OUT)/kernel/driver/smit.ko" "$(OUT)/smit.ko"
server:
	$(MAKE) -C tools OUT="$(OUT)" CC="$(CC)"
package:
	@echo "Ubuntu provides smit.ko and smit ELF; use PLATFORM=openwrt-mt7621 for APK packages."
ubuntu: module server
	python3 tools/build/write_manifest.py "$(OUT)" ubuntu-x86_64 "$$(cat '$(KDIR)/include/config/kernel.release')"
else
all module server package: openwrt
ubuntu:
	@echo "Use PLATFORM=ubuntu-x86_64 for the ubuntu target"; exit 2
endif
openwrt:
	@test -n "$(SDK)" -a -n "$(RESOURCE_CACHE)" || { echo "Set SDK and RESOURCE_CACHE (Linux 6.12.94 archive)."; exit 2; }
	JOBS="$(JOBS)" bash tools/build/openwrt.sh "$(SDK)" "$(RESOURCE_CACHE)" "$(OUT)"
check:
	bash tools/testing/run_offline.sh "$(OUT)/tests"
clean:
	@case "$(abspath $(OUT))" in "$(CURDIR)/build/"*) ;; *) echo "clean requires OUT beneath project build/"; exit 2;; esac
	rm -rf "$(OUT)"
help:
	@echo 'make PLATFORM=ubuntu-x86_64 KDIR=... DVB_USB_V2_DIR=... [CC=gcc-15]'
	@echo 'make PLATFORM=openwrt-mt7621 SDK=... RESOURCE_CACHE=... [JOBS=4]'
	@echo 'make server | module | check; outputs: build/<platform>/smit{,.ko} and OpenWrt APKs'
