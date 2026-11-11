#!/bin/sh
# Buildroot post-build hook. $1 is the target rootfs.
# Publishes os-release and makes sure ai-os-init is PID 1.
set -eu

TARGET_DIR=$1

cat > "${TARGET_DIR}/etc/os-release" <<'EOF'
NAME="AI Native OS"
ID=ai-native-os
VERSION="0.1.0-phase1"
VERSION_ID=0.1.0-phase1
PRETTY_NAME="AI Native OS phase 1"
EOF

if [ ! -f "${TARGET_DIR}/sbin/ai-os-init" ]; then
    echo "post-build: /sbin/ai-os-init is missing" >&2
    exit 1
fi

chmod 755 "${TARGET_DIR}/sbin/ai-os-init"
ln -sfn ai-os-init "${TARGET_DIR}/sbin/init"

if [ -f "${TARGET_DIR}/usr/bin/aios" ]; then
    chmod 755 "${TARGET_DIR}/usr/bin/aios"
fi
