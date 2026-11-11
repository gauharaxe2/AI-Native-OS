AI Native OS — QEMU aarch64 virt

Prefer the repo scripts from the repository root:

  scripts/build.sh
  scripts/run-qemu.sh

The image files are output/images/Image and output/images/rootfs.ext4
(rootfs.ext2 is the same filesystem; ext4 is a symlink Buildroot adds).

Equivalent QEMU invocation, from the repository root, after a build:

  qemu-system-aarch64 \
    -M virt -cpu cortex-a53 -m 512 -smp 1 -nographic \
    -kernel output/images/Image \
    -append "$(cat br2-external/board/qemu-aarch64/cmdline.txt)" \
    -drive file=output/images/rootfs.ext4,if=none,format=raw,id=hd0 \
    -device virtio-blk-device,drive=hd0

Serial is ttyAMA0. The shell is root with no password.
Stop the guest with: poweroff -f
Leave QEMU from the host with: Ctrl-a then x
