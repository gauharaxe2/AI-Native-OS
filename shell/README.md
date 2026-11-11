# Shell

The shell is a BusyBox login shell on the serial console, plus the `aios` command.

| File | Installed as | Role |
| --- | --- | --- |
| `aios` | `/usr/bin/aios` | Status, audit list/show/verify, request, approve, deny, and undo |
| `banner.txt` | `/usr/share/aios/banner.txt` | Banner printed by `ai-os-init` before the shell starts |
| `profile.d.sh` | `/etc/profile.d/aios.sh` | Prompt and one-line hint for login shells |
| `smoke.sh` | `/usr/share/aios/smoke.sh` | In-guest check used by `scripts/run-qemu.sh --smoke` |

`aios` talks to `aios-auditd` through `/usr/bin/aios-audit`. Voice comes later. This phase has no model and no microphone path.
