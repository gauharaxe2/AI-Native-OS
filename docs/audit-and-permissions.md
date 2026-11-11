# Audit and permissions

`aios-auditd` is the phase-2 permission and audit service. `ai-os-init` starts it before the shell. It listens on `/run/aios/audit.sock` and appends JSON lines to `/var/lib/aios/audit.log`. The log is append-only: a later decision is a new record that points at the earlier one with `ref`. Records are not rewritten.

There is still no model runtime. `aios request` is a test client that acts as an agent asking for something.

## Record

Each line is one JSON object:

| Field | Meaning |
| --- | --- |
| `id` | Monotonic record number |
| `timestamp` | UTC time, `YYYY-MM-DDTHH:MM:SSZ` |
| `actor` | Who asked. `aios request` uses `agent`. `approve`, `deny`, and `undo` use `user`. |
| `approver` | Who resolved an `ask`, or empty |
| `action` | Action name, such as `file.create` |
| `target` | Path or other object the action names |
| `risk` | `low`, `medium`, or `high` |
| `decision` | `allowed`, `denied`, or `pending` |
| `result` | What happened: `created /tmp/note.txt`, `waiting`, `denied by policy`, and so on |
| `undo` | How to reverse it, or `irreversible` |
| `ref` | Id of the record this one resolves or undoes, or `0` |
| `prev` | Hash of the previous record, or 64 zeroes for the first |
| `hash` | SHA-256 of the fields above, not including `hash` |

The hash input is those fields joined by newlines, in the order `id`, `timestamp`, `actor`, `approver`, `action`, `target`, `risk`, `decision`, `result`, `undo`, `ref`, `prev`. `aios audit verify` recomputes every hash and checks that each `prev` matches the previous record.

## Policy

`/etc/aios/policy.conf` maps risk levels to `allow`, `ask`, or `deny`, and optionally overrides one action:

```
risk low allow
risk medium ask
risk high ask

action file.create low
action file.delete high
action system.poweroff high deny
```

An action line with three fields uses the decision for its risk. A fourth field forces allow, ask, or deny. Unknown actions are denied and still written to the log.

The default policy, and what the smoke test exercises:

| Request | Risk | Decision | Effect |
| --- | --- | --- | --- |
| `file.create` under `/tmp` | low | allow | Creates the file immediately. Undo deletes it, or restores the previous contents if it replaced a file. |
| `file.delete` under `/tmp` | high | ask | Writes a `pending` record and waits. `aios approve <id>` copies the file to `/var/lib/aios/backups/<id>` and deletes it. `aios deny <id>` writes a denial and leaves the file. |
| `system.poweroff` | high | deny | Logged and refused. The daemon does not power the machine off. |

Paths for the file actions must stay under `/tmp/` and must not contain `..`. That keeps the demo from touching the rest of the root filesystem.

## Commands

```text
aios status
aios audit list
aios audit show <id>
aios audit verify
aios request <action> <target>
aios approve <id>
aios deny <id>
aios undo <id>
```

`ask` requests print `pending id=...` immediately and then block until approve or deny. Run the request in the background when you need the same shell to answer it:

```text
aios request file.delete /tmp/note.txt > /tmp/delete.out &
aios approve 3
```

`aios undo <id>` reads that record's `undo` field. `irreversible` records, including the undo record itself, cannot be reversed. A failed undo is appended as a denied `audit.undo` record so the attempt stays in the chain.

## Socket

Clients send one JSON line and read one JSON line back. A pending request then reads a second line when the user decides. The shell command `/usr/bin/aios-audit` is the same binary as `/usr/sbin/aios-auditd`. The image uses the shell wrapper `/usr/bin/aios`.

Developer builds can point the same binary at a temporary directory with `AIOS_AUDIT_LOG`, `AIOS_AUDIT_SOCK`, `AIOS_AUDIT_POLICY`, and `AIOS_AUDIT_BACKUP`. `scripts/audit-selftest.sh` does that on the host. It does not replace the QEMU smoke test.
