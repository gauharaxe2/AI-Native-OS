#!/usr/bin/env bash
# Host-side check of the audit daemon. No QEMU and no root required.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
work=$(mktemp -d)
trap 'if [[ -n "${pid:-}" ]]; then kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true; fi; rm -rf "$work"; rm -f "$file" "$file2"' EXIT

gcc -std=c11 -Wall -Wextra -Werror -Wno-format-truncation -O2 \
    -o "$work/aios-auditd" \
    "$ROOT/services/aios-auditd/sha256.c" \
    "$ROOT/services/aios-auditd/aios-auditd.c"
ln -s aios-auditd "$work/aios-audit"

file="/tmp/aios-selftest-$$.txt"
file2="/tmp/aios-selftest-$$-b.txt"
rm -f "$file" "$file2"
mkdir -p "$work/backups"

export AIOS_AUDIT_LOG="$work/audit.log"
export AIOS_AUDIT_SOCK="$work/audit.sock"
export AIOS_AUDIT_POLICY="$ROOT/services/aios-auditd/policy.conf"
export AIOS_AUDIT_BACKUP="$work/backups"

"$work/aios-auditd" &
pid=$!
for _ in $(seq 1 50); do
    if [[ -S "$work/audit.sock" ]]; then
        break
    fi
    sleep 0.05
done
if [[ ! -S "$work/audit.sock" ]]; then
    echo "audit-selftest: daemon did not listen" >&2
    exit 1
fi

"$work/aios-audit" status | grep -q '^running$'

create_out=$("$work/aios-audit" request file.create "$file")
printf '%s\n' "$create_out" | grep -q '^allowed '
test -f "$file"
grep -q 'created by aios-auditd' "$file"

deny_out=$("$work/aios-audit" request system.poweroff demo)
printf '%s\n' "$deny_out" | grep -q '^denied '
printf '%s\n' "$deny_out" | grep -q 'denied by policy'

"$work/aios-audit" request file.delete "$file" > "$work/del.out" &
child=$!
for _ in $(seq 1 50); do
    if grep -q '^pending id=' "$work/del.out"; then
        break
    fi
    sleep 0.05
done
grep -q '^pending id=' "$work/del.out"
pending_id=$(sed -n 's/^pending id=\([0-9][0-9]*\).*/\1/p' "$work/del.out" | head -n 1)
"$work/aios-audit" approve "$pending_id" | grep -q '^allowed '
wait "$child"
grep -q '^allowed ' "$work/del.out"
if [[ -e "$file" ]]; then
    echo "audit-selftest: file still exists after approved delete" >&2
    exit 1
fi
exec_id=$(sed -n 's/^allowed id=\([0-9][0-9]*\).*/\1/p' "$work/del.out" | head -n 1)
"$work/aios-audit" undo "$exec_id" | grep -q 'restored '
test -f "$file"

"$work/aios-audit" request file.create "$file2" >/dev/null
"$work/aios-audit" request file.delete "$file2" > "$work/del2.out" &
child=$!
for _ in $(seq 1 50); do
    if grep -q '^pending id=' "$work/del2.out"; then
        break
    fi
    sleep 0.05
done
pending_id=$(sed -n 's/^pending id=\([0-9][0-9]*\).*/\1/p' "$work/del2.out" | head -n 1)
"$work/aios-audit" deny "$pending_id" | grep -q 'denied by user'
wait "$child"
test -f "$file2"

local_out=$("$work/aios-audit" request model.local "What is AI Native OS?")
printf '%s\n' "$local_out" | grep -q '^allowed '
printf '%s\n' "$local_out" | grep -q 'permitted'
"$work/aios-audit" request model.cloud "cloud-stub" > "$work/cloud.out" &
cloud_child=$!
for _ in $(seq 1 50); do
    if grep -q '^pending id=' "$work/cloud.out"; then
        break
    fi
    sleep 0.05
done
grep -q 'model.cloud' "$work/cloud.out"
cloud_id=$(sed -n 's/^pending id=\([0-9][0-9]*\).*/\1/p' "$work/cloud.out" | head -n 1)
"$work/aios-audit" deny "$cloud_id" | grep -q 'denied by user'
wait "$cloud_child"

"$work/aios-audit" list | grep -q 'file.create'
"$work/aios-audit" list | grep -q 'model.local'
"$work/aios-audit" show 1 | grep -q '^decision: allowed$'
"$work/aios-audit" verify | grep -q '^verify ok records='

kill "$pid"
wait "$pid" || true
pid=

python3 - "$work/audit.log" <<'PY'
import pathlib, sys
path = pathlib.Path(sys.argv[1])
data = bytearray(path.read_bytes())
for index, byte in enumerate(data):
    if chr(byte) in "0123456789abcdef" and index > 80:
        data[index] = ord("b" if byte != ord("b") else "c")
        break
else:
    raise SystemExit("no hash digit to tamper")
path.write_bytes(data)
PY
if "$work/aios-audit" verify >/dev/null 2>&1; then
    echo "audit-selftest: tampered log verified" >&2
    exit 1
fi

echo "audit-selftest: ok"
