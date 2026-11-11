# AI Native OS

AI Native OS is an AI-first operating system. It uses the Linux kernel. Everything above the kernel is ours: init, shell, and the system services that will eventually remember context, run an agent, schedule models, and record every AI action so it can be seen and undone.

There is no Android or AOSP userspace, and no GNOME or KDE. A neural processing unit is optional. The system is meant to run on ordinary PCs. QEMU is the first target, then Raspberry Pi 5, then phones that already boot mainline Linux.

The owner of the project is Gauhar Nadeem.

This tree is **phase 1**: a Buildroot external tree that builds a small aarch64 image, boots it on QEMU `virt`, and lands in our own init and a shell. The `aios` command prints system status. No model runtime is included.

## Vision

A person should be able to talk to the computer, or type, and have an agent do the work inside the system they already trust. The agent is not a sidecar app. It is a system service with the same kind of boundaries a careful init system has:

- Memory stays on the device unless the user exports it.
- A local small model is the default. A cloud model runs only after an explicit yes.
- Every action the agent takes is visible, attributable, and reversible.
- The shell is text and voice, not a desktop full of windows.

Phase 1 does not do any of that yet. It makes a honest base: a kernel, an init we control, and a status command, so later services have somewhere real to live.

## Architecture

Layers, from the hardware up:

| Layer | Role | Phase 1 |
| --- | --- | --- |
| Hardware or VM | Ordinary PCs, QEMU, later Pi 5 and mainline phones. NPUs optional. | QEMU aarch64 `virt` |
| Linux kernel | Process, memory, devices, and drivers. | Linux 6.18, Buildroot |
| Base userspace | Our init and a small userland. | `ai-os-init`, BusyBox |
| AI services | Memory, agent runtime, model scheduler, permission and audit. | Not built |
| Shell | Text and voice interface. | `aios` status stub and a login shell |

The layered picture, including services that are only planned, is in [docs/architecture.md](docs/architecture.md).

## Repository layout

```
br2-external/     Buildroot BR2_EXTERNAL tree (defconfig, packages, board files)
services/         System services. Phase 1 ships ai-os-init only.
shell/            Banner, login hint, and the aios CLI.
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
aios status
poweroff -f
```

`scripts/run-qemu.sh --smoke` boots the image, runs `aios status`, powers off, and writes `output/boot.log`. It exits non-zero if the banner and status text never show up.

Images land in `output/images/` (`Image`, `rootfs.ext2`, and a `rootfs.ext4` symlink). The QEMU command is also written in `br2-external/board/qemu-aarch64/readme.txt`.

## Roadmap

1. **Phase 1 (this tree).** Buildroot image, `ai-os-init`, `aios status`, QEMU aarch64 `virt`.
2. **Permission and audit skeleton.** A log of actions with an identity, a reason, and a way to undo, before any agent can act.
3. **Model scheduler.** One local small model through llama.cpp. Cloud escalation only after consent, and recorded by the audit log.
4. **Text shell, then voice.** A text-first shell on top of `aios`, then speech-to-text with whisper.cpp and a text-to-speech path.
5. **Agent runtime.** An agent that can operate programs and the UI, with a confirmation step on anything that changes state.
6. **On-device memory.** A personal memory and context service, permissioned, local by default.
7. **More hardware.** Raspberry Pi 5 board config, then experiments on phones that boot mainline Linux.

## Development image

Phase 1 gives you a root shell on the serial port and does not start a network. Do not ship this image as a daily driver. Later phases add the permission layer before they add capabilities.

## License

TODO: a license has not been chosen. Until a `LICENSE` file exists, do not treat this tree as published under an open-source license, and do not add one without the owner's decision.
