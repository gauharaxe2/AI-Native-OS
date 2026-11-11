################################################################################
#
# aios-auditd
#
################################################################################

# Local sources. The project license is not decided yet; do not add one here.
AIOS_AUDITD_VERSION = 0.2.0
AIOS_AUDITD_SITE = $(BR2_EXTERNAL_AIOS_PATH)/package/aios-auditd
AIOS_AUDITD_SITE_METHOD = local
AIOS_AUDITD_LICENSE = LicenseRef-UNLICENSED

AIOS_REPO = $(abspath $(BR2_EXTERNAL_AIOS_PATH)/..)

define AIOS_AUDITD_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -Wall -Wextra -std=c11 -O2 \
		-o $(@D)/aios-auditd \
		$(AIOS_REPO)/services/aios-auditd/sha256.c \
		$(AIOS_REPO)/services/aios-auditd/aios-auditd.c
endef

define AIOS_AUDITD_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/aios-auditd $(TARGET_DIR)/usr/sbin/aios-auditd
	ln -sfn ../sbin/aios-auditd $(TARGET_DIR)/usr/bin/aios-audit
	$(INSTALL) -D -m 0644 $(AIOS_REPO)/services/aios-auditd/policy.conf \
		$(TARGET_DIR)/etc/aios/policy.conf
endef

$(eval $(generic-package))
