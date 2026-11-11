#!/usr/bin/env bash
# Cheap consistency check for CI and local use. Does not build an image.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"

required=(
    README.md
    CONTRIBUTING.md
    docs/architecture.md
    docs/audit-and-permissions.md
    .gitignore
    services/README.md
    services/ai-os-init/ai-os-init
    services/aios-auditd/aios-auditd.c
    services/aios-auditd/sha256.c
    services/aios-auditd/sha256.h
    services/aios-auditd/policy.conf
    shell/README.md
    shell/aios
    shell/banner.txt
    shell/profile.d.sh
    shell/smoke.sh
    scripts/build.sh
    scripts/run-qemu.sh
    scripts/audit-selftest.sh
    br2-external/external.desc
    br2-external/external.mk
    br2-external/Config.in
    br2-external/configs/aios_qemu_aarch64_defconfig
    br2-external/board/qemu-aarch64/linux.config
    br2-external/board/qemu-aarch64/busybox.fragment
    br2-external/board/qemu-aarch64/cmdline.txt
    br2-external/board/qemu-aarch64/post-build.sh
    br2-external/board/qemu-aarch64/readme.txt
    br2-external/board/qemu-aarch64/patches/linux/linux.hash
    br2-external/package/ai-os-init/Config.in
    br2-external/package/ai-os-init/ai-os-init.mk
    br2-external/package/aios/Config.in
    br2-external/package/aios/aios.mk
    br2-external/package/aios-auditd/Config.in
    br2-external/package/aios-auditd/aios-auditd.mk
    .github/workflows/ci.yml
)

missing=0
for path in "${required[@]}"; do
    if [[ ! -f "$path" ]]; then
        printf 'check-tree: missing %s\n' "$path" >&2
        missing=1
    fi
done
if [[ "$missing" -ne 0 ]]; then
    exit 1
fi

grep -q '^name: AIOS$' br2-external/external.desc
grep -q '^desc: ' br2-external/external.desc

defconfig=br2-external/configs/aios_qemu_aarch64_defconfig
for key in \
    'BR2_aarch64=y' \
    'BR2_TOOLCHAIN_EXTERNAL_ARM_AARCH64=y' \
    'BR2_INIT_NONE=y' \
    'BR2_LINUX_KERNEL_CUSTOM_VERSION_VALUE="6.18.7"' \
    'BR2_LINUX_KERNEL_IMAGE=y' \
    'BR2_PACKAGE_AI_OS_INIT=y' \
    'BR2_PACKAGE_AIOS=y' \
    'BR2_PACKAGE_AIOS_AUDITD=y' \
    'BR2_TARGET_ROOTFS_EXT2_4=y'
do
    if ! grep -q -F "$key" "$defconfig"; then
        printf 'check-tree: defconfig missing %s\n' "$key" >&2
        exit 1
    fi
done

if [[ -e LICENSE || -e LICENSE.md || -e COPYING || -e COPYING.md ]]; then
    printf 'check-tree: a license file is present, but the license is still a TODO\n' >&2
    exit 1
fi

if ! grep -q 'TODO' README.md || ! grep -q -i 'license' README.md; then
    printf 'check-tree: README must keep the license TODO\n' >&2
    exit 1
fi

if grep -n '\[\[' services/ai-os-init/ai-os-init shell/aios shell/profile.d.sh shell/smoke.sh; then
    printf 'check-tree: device shell scripts must stay POSIX\n' >&2
    exit 1
fi

if ! grep -q '```mermaid' docs/architecture.md; then
    printf 'check-tree: architecture doc needs a mermaid diagram\n' >&2
    exit 1
fi

if ! grep -q 'linux-6.18.7.tar.xz' br2-external/board/qemu-aarch64/patches/linux/linux.hash; then
    printf 'check-tree: kernel hash does not match the pinned version\n' >&2
    exit 1
fi

while IFS= read -r -d '' file; do
    if head -n 1 "$file" | grep -q '^#!'; then
        bash -n "$file"
    fi
done < <(find scripts shell services br2-external -type f -print0)

# The status CLI should run on the host as well as in the image.
status_out=$(sh shell/aios status)
printf '%s\n' "$status_out" | grep -q 'AI Native OS'
printf '%s\n' "$status_out" | grep -q 'not installed'
sh shell/aios version | grep -q '0.2.0-phase2'
scripts/audit-selftest.sh
if sh shell/aios definitely-not-a-command >/dev/null 2>&1; then
    printf 'check-tree: aios accepted an unknown command\n' >&2
    exit 1
fi

printf 'check-tree: ok\n'
