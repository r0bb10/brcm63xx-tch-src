#
# Copyright (C) 2021 Luca Piccirillo <luca.piccirillo@gmail.com>
#
# This is free software, licensed under the GNU General Public License v2.
# See /LICENSE for more information.
#

include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=lime
PKG_VERSION:=1.9.1
PKG_SOURCE_PROTO:=git
PKG_SOURCE:=$(PKG_NAME)-$(PKG_VERSION).tar.gz
PKG_SOURCE_URL:=https://github.com/504ensicsLabs/LiME.git
PKG_SOURCE_DATE:=2020-08-09
PKG_SOURCE_VERSION:=fa37b698fdaacea6d2cfd99253e13979fbd38bb8
PKG_SOURCE_SUBDIR:=$(PKG_NAME)-$(PKG_VERSION)

PKG_BUILD_DIR:=$(KERNEL_BUILD_DIR)/$(PKG_NAME)-$(PKG_VERSION)

include $(INCLUDE_DIR)/package.mk
include $(INCLUDE_DIR)/package-defaults.mk

define KernelPackage/lime
  SUBMENU:=Other modules
  TITLE:=LiME ~ Linux Memory Extractor
  FILES:=$(PKG_BUILD_DIR)/src/lime.ko
  KCONFIG:=
  MAINTAINER:=Luca Piccirillo <luca.piccirillo@gmail.com>
endef

define KernelPackage/lime/description
 A Loadable Kernel Module (LKM) which allows for volatile memory acquisition from Linux and Linux-based devices,
such as Android. This makes LiME unique as it is the first tool that allows for full memory captures on Android devices.
It also minimizes its interaction between user and kernel space processes during acquisition, which allows it to
produce memory captures that are more forensically sound than those of other tools designed for Linux memory acquisition.
endef

MAKE_OPTS:= \
	ARCH="$(LINUX_KARCH)" \
	CROSS_COMPILE="$(TARGET_CROSS)" \
	SUBDIRS="$(PKG_BUILD_DIR)/src" \
	M="$(PKG_BUILD_DIR)/src" \
	$(EXTRA_KCONFIG)

define Build/Compile
	$(MAKE) -C "$(LINUX_DIR)" \
		$(MAKE_OPTS) \
		modules
endef

define Build/InstallDev
	$(INSTALL_DIR) $(STAGING_DIR)/usr/include
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/src/*.h $(STAGING_DIR)/usr/include
endef

$(eval $(call KernelPackage,lime))
