# AI Native OS

AI Native OS is an AI-first operating system. It uses the Linux kernel. Everything above the kernel is ours: init, shell, and the system services that will eventually remember context, run an agent, schedule models, and record every AI action so it can be seen and undone.

There is no Android or AOSP userspace, and no GNOME or KDE. A neural processing unit is optional. The system is meant to run on ordinary PCs. QEMU is the first target, then Raspberry Pi 5, then phones that already boot mainline Linux.

The owner of the project is Gauhar Nadeem.

This tree is **phase 3**. It boots a small aarch64 image, records actions in `aios-auditd`, and runs a local model through `aios-modeld`. Cloud inference is an audit stub only.

## Vision

A person should be able to talk to the computer, or type, and have an agent do the work inside the system they already trust. The agent is not a sidecar app. It is a system service with the same kind of boundaries a careful init system has:

- Memory stays on the device unless the user exports it.
- A local small model is the default. A cloud model runs only after an explicit yes.
- Every action the agent takes is visible, attributable, and reversible.
- The shell is text and voice, not a desktop full of windows.

Phase 2 records actions and can undo the demo file operations. Phase 3 runs a small local model and logs that call. Memory, the agent, and voice are still later.

## Architecture

Layers, from the hardware up:

| Layer | Role | Phase 1 |
| --- | --- | --- |
| Hardware or VM | Ordinary PCs, QEMU, later Pi 5 and mainline phones. NPUs optional. | QEMU aarch64 `virt` |
| Linux kernel | Process, memory, devices, and drivers. | Linux 6.18, Buildroot |
| Base userspace | Our init and a small userland. | `ai-os-init`, BusyBox |
| AI services | Memory, agent runtime, model scheduler, permission and audit. | Audit plus a local scheduler. See [docs/audit-and-permissions.md](docs/audit-and-permissions.md) and [docs/model-scheduler.md](docs/model-scheduler.md). |
| Shell | Text and voice interface. | `aios` status, audit, request, approve, deny, and undo. |

The layered picture, including services that are only planned, is in [docs/architecture.md](docs/architecture.md).

## Repository layout

```
br2-external/     Buildroot BR2_EXTERNAL tree (defconfig, packages, board files)
services/         System services. ai-os-init and aios-auditd.
shell/            Banner, login hint, aios CLI, and the in-guest smoke script.
scripts/          build.sh, run-qemu.sh, and a tree check used by CI.
docs/             Architecture and design notes.
.github/          Sanity-check workflow. It does not build the image.
```

Buildroot itself is not vendored. `scripts/build.sh` downloads a pinned release into `.buildroot/` and writes the image to `output/`. Both directories are gitignored.

## Build and run

Host tools: a C toolchain (`build-essential`), `bc`, `bison`, `flex`, `cpio`, `rsync`, `unzip`, `file`, `wget`, `libssl-dev`, `libelf-dev`, `python3`, and `qemu-system-aarch64` (Ubuntu package `qemu-system-arm`). The defconfig downloads the Arm GNU AArch64 toolchain, which Buildroot publishes for x86_64 and aarch64 hosts.

```bash
scripts/build.sh
scripts/run-qemu.sh
```

The serial console is the UI. After the banner, the shell is root with no password. That is a development image, not a locked-down system.

```text
scripts/fetch-model.sh
scripts/build.sh
scripts/run-qemu.sh --smoke
```

Inside the guest:

```text
aios status
aios model status
aios ask "What is AI Native OS?"
aios audit verify
poweroff -f
```

`scripts/run-qemu.sh --smoke` boots the image with 3 GB of RAM and runs `/usr/share/aios/smoke.sh`. That script checks the audit file actions, waits for the local model, asks it one short question, leaves a `model.cloud` request pending, and checks the hash chain. The serial log is `output/boot.log`. Run `scripts/fetch-model.sh` before the smoke test so `models/model.img` exists.

Images land in `output/images/` (`Image`, `rootfs.ext2`, and a `rootfs.ext4` symlink). The QEMU command is also written in `br2-external/board/qemu-aarch64/readme.txt`.

## Roadmap

1. **Phase 1.** Buildroot image, `ai-os-init`, `aios status`, QEMU aarch64 `virt`.
2. **Permission and audit.** `aios-auditd`, a hash-chained log, allow/ask/deny policy, and undo for the demo file actions. Details are in [docs/audit-and-permissions.md](docs/audit-and-permissions.md).
3. **Model scheduler (this tree).** llama.cpp on CPU, `aios ask` for a local SmolLM2 model, and `model.cloud` as an ask-only audit stub. See [docs/model-scheduler.md](docs/model-scheduler.md).
4. **Text shell, then voice.** A text-first shell on top of `aios`, then speech-to-text with whisper.cpp and a text-to-speech path.
5. **Agent runtime.** An agent that can operate programs and the UI, with a confirmation step on anything that changes state.
6. **On-device memory.** A personal memory and context service, permissioned, local by default.
7. **More hardware.** Raspberry Pi 5 board config, then experiments on phones that boot mainline Linux.

## Development image

The image gives you a root shell on the serial port and does not start a network. The audit policy is a development policy: low-risk file creates are allowed, high-risk deletes wait for confirmation, and `system.poweroff` is denied. Do not ship this image as a daily driver.

## License

TODO: a license has not been chosen. Until a `LICENSE` file exists, do not treat this tree as published under an open-source license, and do not add one without the owner's decision.
