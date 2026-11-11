#!/usr/bin/env bash
# Download pinned Buildroot and build the AI Native OS QEMU image.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILDROOT_VERSION=${BUILDROOT_VERSION:-2026.08.1}
CACHE=${BUILDROOT_CACHE:-"$ROOT/.buildroot"}
BUILDROOT_DIR=${BUILDROOT_DIR:-"$CACHE/buildroot-$BUILDROOT_VERSION"}
OUT=${OUT:-"$ROOT/output"}
DL_DIR=${BR2_DL_DIR:-"$CACHE/dl"}
DEFCONFIG=${DEFCONFIG:-aios_qemu_aarch64_defconfig}
JOBS=${JOBS:-$(nproc)}
EXTERNAL="$ROOT/br2-external"

need() {
    if ! command -v "$1" >/dev/null 2>&1; then
        printf 'build.sh: missing host tool %s\n' "$1" >&2
        printf 'build.sh: see README "Build and run" for the package list\n' >&2
        exit 1
    fi
}

for tool in make gcc g++ wget tar rsync cpio bison flex bc python3; do
    need "$tool"
done

if [[ ! -f "$EXTERNAL/external.desc" ]]; then
    printf 'build.sh: BR2_EXTERNAL tree not found at %s\n' "$EXTERNAL" >&2
    exit 1
fi

mkdir -p "$CACHE" "$DL_DIR" "$OUT"

if [[ ! -f "$BUILDROOT_DIR/Makefile" ]]; then
    tarball="$CACHE/buildroot-$BUILDROOT_VERSION.tar.gz"
    url="https://github.com/buildroot/buildroot/archive/refs/tags/${BUILDROOT_VERSION}.tar.gz"
    printf 'build.sh: downloading Buildroot %s\n' "$BUILDROOT_VERSION"
    wget -O "$tarball.partial" "$url"
    mv "$tarball.partial" "$tarball"
    tar -xzf "$tarball" -C "$CACHE"
fi

if [[ ! -f "$BUILDROOT_DIR/Makefile" ]]; then
    printf 'build.sh: Buildroot Makefile missing at %s\n' "$BUILDROOT_DIR" >&2
    exit 1
fi

export BR2_DL_DIR="$DL_DIR"

make -C "$BUILDROOT_DIR" \
    BR2_EXTERNAL="$EXTERNAL" \
    O="$OUT" \
    "$DEFCONFIG"

make -C "$BUILDROOT_DIR" \
    BR2_EXTERNAL="$EXTERNAL" \
    O="$OUT" \
    -j"$JOBS"

printf 'build.sh: images in %s\n' "$OUT/images"
ls -lh "$OUT/images"
