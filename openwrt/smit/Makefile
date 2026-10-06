include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=smit
PKG_VERSION:=0.1
PKG_RELEASE:=5
PKG_SOURCE:=linux-6.12.94.tar.xz
PKG_SOURCE_URL:=https://cdn.kernel.org/pub/linux/kernel/v6.x/
PKG_HASH:=e998a232b9418db3301cb58468e291a4f41d6ab8306029b30d991f56251dc8d2
PKG_BUILD_DIR:=$(KERNEL_BUILD_DIR)/$(PKG_NAME)-$(PKG_VERSION)
PKG_BUILD_PARALLEL:=1
SMIT_SOURCE_DIR?=$(CURDIR)/src

include $(INCLUDE_DIR)/package.mk

define KernelPackage/dvb-core
  SUBMENU:=Video Support
  TITLE:=Linux DVB core (external module)
  FILES:=$(PKG_BUILD_DIR)/drivers/media/dvb-core/dvb-core.ko
  AUTOLOAD:=$(call AutoProbe,dvb-core)
endef

define KernelPackage/dvb-usb-v2
  SUBMENU:=Video Support
  TITLE:=Linux DVB USB v2 framework (external module)
  DEPENDS:=+kmod-usb-core +kmod-dvb-core
  FILES:=$(PKG_BUILD_DIR)/drivers/media/usb/dvb-usb-v2/dvb_usb_v2.ko
  AUTOLOAD:=$(call AutoProbe,dvb_usb_v2)
endef

define KernelPackage/smit
  SUBMENU:=Video Support
  TITLE:=SMIT iCast USB-C/DTMB USB Tuner
  DEPENDS:=+kmod-dvb-usb-v2
  FILES:=$(PKG_BUILD_DIR)/driver/smit.ko
  AUTOLOAD:=$(call AutoProbe,smit)
endef

define Package/smit
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=SMIT iCast USB-C/DTMB USB Tuner
  DEPENDS:=+libc +libpthread
endef

define Package/smit/conffiles
/etc/config/smit
endef

define Build/Prepare
	test "$(LINUX_VERSION)" = "6.12.94"
	! grep -Eq '^CONFIG_(DVB_CORE|RC_CORE|MEDIA_CONTROLLER_DVB|DVB_NET|DVB_MMAP)=' $(LINUX_DIR)/.config
	mkdir -p $(PKG_BUILD_DIR)
	tar -xJf $(DL_DIR)/$(PKG_SOURCE) -C $(PKG_BUILD_DIR) --strip-components=1 \
		linux-6.12.94/drivers/media/dvb-core \
		linux-6.12.94/drivers/media/usb/dvb-usb-v2
	$(CP) $(SMIT_SOURCE_DIR)/driver $(PKG_BUILD_DIR)/driver
	$(CP) $(SMIT_SOURCE_DIR)/include $(PKG_BUILD_DIR)/include
	$(CP) $(SMIT_SOURCE_DIR)/tools $(PKG_BUILD_DIR)/tools
	$(CP) ./Kbuild $(PKG_BUILD_DIR)/Makefile
	$(CP) ./dvb-usb-v2.Kbuild $(PKG_BUILD_DIR)/drivers/media/usb/dvb-usb-v2/Kbuild
endef

define Build/Compile
	$(KERNEL_MAKE) M=$(PKG_BUILD_DIR) \
		CONFIG_DVB_CORE=m CONFIG_DVB_USB_V2=m \
		DVB_USB_V2_DIR=$(PKG_BUILD_DIR)/drivers/media/usb/dvb-usb-v2 \
		W=1 modules
	+$(MAKE) -C $(PKG_BUILD_DIR)/tools OUT=$(PKG_BUILD_DIR)/bin \
		CC="$(TARGET_CC)" CPPFLAGS="$(TARGET_CPPFLAGS) -D_FILE_OFFSET_BITS=64" \
		CFLAGS="$(TARGET_CFLAGS) -Wall -Wextra -Werror" LDFLAGS="$(TARGET_LDFLAGS)"
endef

define Package/smit/install
	$(INSTALL_DIR) $(1)/usr/bin $(1)/etc/init.d $(1)/etc/config
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/bin/smit $(1)/usr/bin/smit
	$(INSTALL_BIN) ./files/smit.init $(1)/etc/init.d/smit
	$(INSTALL_CONF) ./files/smit.config $(1)/etc/config/smit
endef

$(eval $(call KernelPackage,dvb-core))
$(eval $(call KernelPackage,dvb-usb-v2))
$(eval $(call KernelPackage,smit))
$(eval $(call BuildPackage,smit))
