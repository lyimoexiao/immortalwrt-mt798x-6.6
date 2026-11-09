include $(TOPDIR)/rules.mk

PKG_NAME:=wifi-led
PKG_RELEASE:=1

include $(INCLUDE_DIR)/package.mk

define Package/wifi-led
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=Single Wi-Fi activity LED daemon
endef

define Package/wifi-led/description
  Drives one Wi-Fi LED from the packet activity of both radios (ra0/rax0).
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-o $(PKG_BUILD_DIR)/wifi-led $(PKG_BUILD_DIR)/wifi-led.c
endef

define Package/wifi-led/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/etc/init.d
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/wifi-led $(1)/usr/sbin/wifi-led
	$(INSTALL_BIN) ./files/wifi-led.init $(1)/etc/init.d/wifi-led
endef

$(eval $(call BuildPackage,wifi-led))
