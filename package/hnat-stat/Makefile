include $(TOPDIR)/rules.mk

PKG_NAME:=hnat-stat
PKG_RELEASE:=1

include $(INCLUDE_DIR)/package.mk

define Package/hnat-stat
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=MediaTek HNAT/WED traffic accounting daemon
  DEPENDS:=@TARGET_mediatek
endef

define Package/hnat-stat/description
  Reads offloaded flow entries from the MediaTek HNAT/PPE tables and keeps
  cumulative byte counts so that live rates include accelerated traffic.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-o $(PKG_BUILD_DIR)/hnat-stat $(PKG_BUILD_DIR)/hnat-stat.c
endef

define Package/hnat-stat/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/etc/init.d
	$(INSTALL_DIR) $(1)/usr/share/rpcd/ucode $(1)/usr/share/rpcd/acl.d
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/hnat-stat $(1)/usr/sbin/hnat-stat
	$(INSTALL_BIN) ./files/hnat-stat.init $(1)/etc/init.d/hnat-stat
	$(INSTALL_DATA) ./files/hnat.uc $(1)/usr/share/rpcd/ucode/hnat.uc
	$(INSTALL_DATA) ./files/hnat.acl.json $(1)/usr/share/rpcd/acl.d/hnat.json
endef

$(eval $(call BuildPackage,hnat-stat))
