#!/bin/sh
# In-guest smoke test for the permission and audit layer.
# POSIX sh for BusyBox ash. Prints SMOKE_* markers and then powers off.

fail() {
    printf '%s\n' "SMOKE_FAIL: $1"
    poweroff -f
    exit 1
}

i=0
while [ ! -S /run/aios/audit.sock ]; do
    i=$((i + 1))
    if [ "$i" -gt 5 ]; then
        fail "audit socket missing"
    fi
    sleep 1
done

printf '%s\n' "--- status ---"
aios status
aios status | grep 'audit      running' >/dev/null || fail "audit not running"
printf '%s\n' "SMOKE_STATUS_OK"

demo=/tmp/aios-demo.txt
rm -f "$demo"

printf '%s\n' "--- low risk: file.create (allow) ---"
aios request file.create "$demo" > /tmp/smoke-create.out || fail "create command failed"
cat /tmp/smoke-create.out
grep '^allowed ' /tmp/smoke-create.out >/dev/null || fail "create was not allowed"
test -f "$demo" || fail "created file missing"
printf '%s\n' "SMOKE_ALLOW_OK"

printf '%s\n' "--- high risk override: system.poweroff (deny) ---"
aios request system.poweroff demo > /tmp/smoke-deny.out || fail "deny command failed"
cat /tmp/smoke-deny.out
grep '^denied ' /tmp/smoke-deny.out >/dev/null || fail "poweroff was not denied"
grep 'denied by policy' /tmp/smoke-deny.out >/dev/null || fail "missing policy denial"
printf '%s\n' "SMOKE_DENY_OK"

printf '%s\n' "--- high risk: file.delete (ask, then approve) ---"
aios request file.delete "$demo" > /tmp/smoke-delete.out &
child=$!
i=0
while ! grep '^pending id=' /tmp/smoke-delete.out >/dev/null 2>&1; do
    i=$((i + 1))
    if [ "$i" -gt 5 ]; then
        fail "delete request did not become pending"
    fi
    sleep 1
done
cat /tmp/smoke-delete.out
pending=$(grep '^pending id=' /tmp/smoke-delete.out | head -n 1)
pending=${pending#pending id=}
pending=${pending%% *}
aios approve "$pending" || fail "approve failed"
wait "$child" || fail "delete client failed"
printf '%s\n' "--- delete result ---"
cat /tmp/smoke-delete.out
grep '^allowed ' /tmp/smoke-delete.out >/dev/null || fail "approved delete was not allowed"
if [ -e "$demo" ]; then
    fail "file still present after delete"
fi
printf '%s\n' "SMOKE_APPROVE_OK"

exec_id=$(grep '^allowed id=' /tmp/smoke-delete.out | head -n 1)
exec_id=${exec_id#allowed id=}
exec_id=${exec_id%% *}
printf '%s\n' "--- undo delete ${exec_id} ---"
aios undo "$exec_id" || fail "undo failed"
test -f "$demo" || fail "undo did not restore the file"
grep 'created by aios-auditd' "$demo" >/dev/null || fail "restored contents mismatch"
printf '%s\n' "SMOKE_UNDO_OK"

printf '%s\n' "--- audit list ---"
aios audit list || fail "audit list failed"
printf '%s\n' "--- audit verify ---"
aios audit verify || fail "audit verify failed"
printf '%s\n' "SMOKE_VERIFY_OK"
printf '%s\n' "SMOKE_OK"
poweroff -f
