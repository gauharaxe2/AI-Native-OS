# Services

Long-running system programs live here. Buildroot packages under `br2-external/package/` install them into the image. The packages stay thin so the sources can be read without opening a Buildroot tree.

## Phase 1

`ai-os-init/` is PID 1 for the development image. It mounts the early filesystems, prints the welcome banner, and respawns a shell. It does not supervise AI workloads. Those do not exist yet.

## Planned

These are not in the tree yet. Add them as their own directories when the work starts, and only after the permission and audit layer exists for anything that can change the system:

| Service | Role |
| --- | --- |
| Memory and context | On-device, permissioned personal memory |
| Agent runtime | Operates programs and the UI, with confirmation |
| Model scheduler | Local small model via llama.cpp; cloud only with consent |
| Permission and audit | Makes every AI action visible and reversible |
| Voice | whisper.cpp speech-to-text and text-to-speech, beside the text shell |

Do not add model code, weights, or network clients in phase 1.
