################################################################################
#
# aios-modeld
#
################################################################################

# Local sources. The project license is not decided yet; do not add one here.
AIOS_MODELD_VERSION = 0.3.0
AIOS_MODELD_SITE = $(BR2_EXTERNAL_AIOS_PATH)/package/aios-modeld
AIOS_MODELD_SITE_METHOD = local
AIOS_MODELD_LICENSE = LicenseRef-UNLICENSED

AIOS_REPO = $(abspath $(BR2_EXTERNAL_AIOS_PATH)/..)

define AIOS_MODELD_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -Wall -Wextra -std=c11 -O2 \
		-o $(@D)/aios-modeld \
		$(AIOS_REPO)/services/aios-modeld/aios-modeld.c
endef

define AIOS_MODELD_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/aios-modeld $(TARGET_DIR)/usr/sbin/aios-modeld
	ln -sfn ../sbin/aios-modeld $(TARGET_DIR)/usr/bin/aios-model
endef

$(eval $(generic-package))
