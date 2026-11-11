################################################################################
#
# ai-os-init
#
################################################################################

# Local sources. The project license is not decided yet; do not add one here.
AI_OS_INIT_VERSION = 0.1.0
AI_OS_INIT_SITE = $(BR2_EXTERNAL_AIOS_PATH)/package/ai-os-init
AI_OS_INIT_SITE_METHOD = local
AI_OS_INIT_LICENSE = LicenseRef-UNLICENSED
AI_OS_INIT_DEPENDENCIES = busybox

AIOS_REPO = $(abspath $(BR2_EXTERNAL_AIOS_PATH)/..)

define AI_OS_INIT_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(AIOS_REPO)/services/ai-os-init/ai-os-init \
		$(TARGET_DIR)/sbin/ai-os-init
	ln -sfn ai-os-init $(TARGET_DIR)/sbin/init
endef

$(eval $(generic-package))
