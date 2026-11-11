################################################################################
#
# aios
#
################################################################################

# Local sources. The project license is not decided yet; do not add one here.
AIOS_VERSION = 0.1.0
AIOS_SITE = $(BR2_EXTERNAL_AIOS_PATH)/package/aios
AIOS_SITE_METHOD = local
AIOS_LICENSE = LicenseRef-UNLICENSED
AIOS_DEPENDENCIES = busybox

AIOS_REPO = $(abspath $(BR2_EXTERNAL_AIOS_PATH)/..)

define AIOS_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(AIOS_REPO)/shell/aios $(TARGET_DIR)/usr/bin/aios
	$(INSTALL) -D -m 0644 $(AIOS_REPO)/shell/banner.txt \
		$(TARGET_DIR)/usr/share/aios/banner.txt
	$(INSTALL) -D -m 0644 $(AIOS_REPO)/shell/profile.d.sh \
		$(TARGET_DIR)/etc/profile.d/aios.sh
	$(INSTALL) -D -m 0755 $(AIOS_REPO)/shell/smoke.sh \
		$(TARGET_DIR)/usr/share/aios/smoke.sh
endef

$(eval $(generic-package))
