# Services

Long-running system programs live here. Buildroot packages under `br2-external/package/` install them into the image. The packages stay thin so the sources can be read without opening a Buildroot tree.

## Present

`ai-os-init/` is PID 1. It mounts the early filesystems, starts `aios-auditd`, prints the banner, and respawns a shell.

`aios-auditd/` is the permission and audit daemon. It keeps the hash-chained log and applies `/etc/aios/policy.conf`. See [docs/audit-and-permissions.md](../docs/audit-and-permissions.md).

## Planned

| Service | Role |
| --- | --- |
| Memory and context | On-device, permissioned personal memory |
| Agent runtime | Operates programs and the UI, with confirmation, and must call the audit daemon |
| Model scheduler | Local small model via llama.cpp; cloud only with consent, recorded in the audit log |
| Voice | whisper.cpp speech-to-text and text-to-speech, beside the text shell |

Do not add model code or weights yet.
