# Shell

The phase-1 shell is a BusyBox login shell on the serial console, plus a status command.

| File | Installed as | Role |
| --- | --- | --- |
| `aios` | `/usr/bin/aios` | Prints version, kernel, uptime, init state, and the list of services that are not installed yet |
| `banner.txt` | `/usr/share/aios/banner.txt` | Banner printed by `ai-os-init` before the shell starts |
| `profile.d.sh` | `/etc/profile.d/aios.sh` | Prompt and one-line hint for login shells |

Voice (whisper.cpp and text-to-speech) and a richer text shell come later. They should land in this directory, not inside the init script. Phase 1 has no model and no microphone path.
