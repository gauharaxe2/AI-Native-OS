#!/usr/bin/env bash
# Boot the phase-1 image on QEMU aarch64 virt.
#   scripts/run-qemu.sh            interactive serial console
#   scripts/run-qemu.sh --smoke    boot, run `aios status`, power off
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
IMAGES=${IMAGES:-"$ROOT/output/images"}
MEMORY=${MEMORY:-512}
SMP=${SMP:-1}
QEMU=${QEMU:-qemu-system-aarch64}
CMDLINE_FILE=${CMDLINE_FILE:-"$ROOT/br2-external/board/qemu-aarch64/cmdline.txt"}
SMOKE_LOG=${SMOKE_LOG:-"$ROOT/output/boot.log"}
SMOKE_TIMEOUT=${SMOKE_TIMEOUT:-120}
SMOKE=0
EXTRA=()

usage() {
    cat <<'EOF'
Usage: scripts/run-qemu.sh [--smoke] [-- qemu-args...]

Boots output/images/Image with the ext4 rootfs on QEMU aarch64 virt.
--smoke runs `aios status`, then `poweroff -f`, and writes output/boot.log.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --smoke)
            SMOKE=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --)
            shift
            EXTRA+=("$@")
            break
            ;;
        *)
            printf 'run-qemu.sh: unknown argument %s\n' "$1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if ! command -v "$QEMU" >/dev/null 2>&1; then
    printf 'run-qemu.sh: %s not found (Ubuntu package: qemu-system-arm)\n' "$QEMU" >&2
    exit 1
fi

KERNEL="$IMAGES/Image"
if [[ -f "$IMAGES/rootfs.ext4" ]]; then
    ROOTFS="$IMAGES/rootfs.ext4"
elif [[ -f "$IMAGES/rootfs.ext2" ]]; then
    ROOTFS="$IMAGES/rootfs.ext2"
else
    printf 'run-qemu.sh: no rootfs in %s\n' "$IMAGES" >&2
    printf 'run-qemu.sh: run scripts/build.sh first\n' >&2
    exit 1
fi

if [[ ! -f "$KERNEL" ]]; then
    printf 'run-qemu.sh: kernel image missing: %s\n' "$KERNEL" >&2
    exit 1
fi

if [[ ! -f "$CMDLINE_FILE" ]]; then
    printf 'run-qemu.sh: cmdline file missing: %s\n' "$CMDLINE_FILE" >&2
    exit 1
fi

APPEND=$(tr '\n' ' ' < "$CMDLINE_FILE" | sed 's/[[:space:]]*$//')

QEMU_ARGS=(
    -M virt
    -cpu cortex-a53
    -m "$MEMORY"
    -smp "$SMP"
    -nographic
    -kernel "$KERNEL"
    -append "$APPEND"
    -drive "file=${ROOTFS},if=none,format=raw,id=hd0"
    -device virtio-blk-device,drive=hd0
)

if [[ "$SMOKE" -eq 0 ]]; then
    exec "$QEMU" "${QEMU_ARGS[@]}" "${EXTRA[@]}"
fi

mkdir -p "$(dirname "$SMOKE_LOG")"

python3 - "$QEMU" "$SMOKE_LOG" "$SMOKE_TIMEOUT" "${QEMU_ARGS[@]}" "${EXTRA[@]}" <<'PY'
import os
import pty
import select
import subprocess
import sys
import time

qemu = sys.argv[1]
log_path = sys.argv[2]
timeout = int(sys.argv[3])
cmd = sys.argv[4:]

master, slave = pty.openpty()
proc = subprocess.Popen(
    [qemu, *cmd],
    stdin=slave,
    stdout=slave,
    stderr=slave,
    close_fds=True,
)
os.close(slave)

buf = b""
sent_status = False
sent_off = False
deadline = time.time() + timeout

def marker_ok(data: bytes) -> bool:
    return (
        b"console shell ready" in data
        and b"model:" in data
        and b"not installed" in data
        and b"ai-os-init (booted)" in data
    )

while time.time() < deadline:
    if proc.poll() is not None:
        ready, _, _ = select.select([master], [], [], 0)
        if not ready:
            break
    ready, _, _ = select.select([master], [], [], 0.5)
    if not ready:
        continue
    try:
        chunk = os.read(master, 4096)
    except OSError:
        break
    if not chunk:
        break
    buf += chunk
    sys.stdout.buffer.write(chunk)
    sys.stdout.buffer.flush()
    if not sent_status and b"console shell ready" in buf:
        time.sleep(0.6)
        os.write(master, b"aios status\n")
        sent_status = True
    if sent_status and not sent_off and marker_ok(buf):
        time.sleep(0.3)
        os.write(master, b"poweroff -f\n")
        sent_off = True
        deadline = min(deadline, time.time() + 20)

# Drain whatever is left after the guest exits or the deadline hits.
end = time.time() + 2
while time.time() < end:
    ready, _, _ = select.select([master], [], [], 0.2)
    if not ready:
        if proc.poll() is not None:
            break
        continue
    try:
        chunk = os.read(master, 4096)
    except OSError:
        break
    if not chunk:
        break
    buf += chunk
    sys.stdout.buffer.write(chunk)
    sys.stdout.buffer.flush()

if proc.poll() is None:
    proc.terminate()
    time.sleep(1)
    if proc.poll() is None:
        proc.kill()
        proc.wait(timeout=5)

os.close(master)
with open(log_path, "wb") as handle:
    handle.write(buf)

if marker_ok(buf):
    print(f"\nrun-qemu.sh: smoke boot ok, log written to {log_path}", file=sys.stderr)
    sys.exit(0)

print(f"\nrun-qemu.sh: smoke boot failed, log written to {log_path}", file=sys.stderr)
sys.exit(1)
PY
