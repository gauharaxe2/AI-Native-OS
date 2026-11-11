# Contributing

AI Native OS is owned by Gauhar Nadeem. Phase 1 is a bootable base. Later phases add memory, an agent, a model scheduler, audit, and voice. Do not start those by dropping model code or weights into this tree.

## Before you change code

- Read [docs/architecture.md](docs/architecture.md). Keep new work inside the layer it belongs to.
- The license is not decided. Do not add a `LICENSE` file, and do not relicense snippets, until the owner chooses one.
- Do not vendor Buildroot, toolchains, the kernel source, or model weights. `scripts/build.sh` downloads a pinned Buildroot release.
- Do not commit `.buildroot/` or `output/`.

## Layout

| Path | What goes there |
| --- | --- |
| `services/` | Long-running system programs. `ai-os-init` is the phase-1 init. |
| `shell/` | The human interface: banner, profile snippet, `aios`. |
| `br2-external/` | Buildroot packages and board files that install the sources above. Keep packages thin. |
| `scripts/` | Host-side build, run, and check scripts. |
| `docs/` | Design notes. |

Shell scripts that run **on the device** (`services/ai-os-init/ai-os-init`, `shell/aios`) are POSIX `sh` and must work under BusyBox ash. No `[[`, no bash arrays. Host scripts under `scripts/` may use bash.

## Build

Host packages and the exact commands are in the README under "Build and run". After a change to init, the CLI, or the defconfig:

```bash
scripts/check-tree.sh
scripts/build.sh
scripts/run-qemu.sh --smoke
```

`scripts/build.sh` accepts `JOBS` (default: the processor count) and `BUILDROOT_VERSION` (default: the pin at the top of the script).

## CI

`.github/workflows/ci.yml` runs `scripts/check-tree.sh` only. It does not download Buildroot or compile a kernel. A green check means the tree is internally consistent, not that the image boots.

## Commit style

Write commit messages that say what changed and why. Keep phase-1 patches limited to the base image, the init, the status CLI, docs, and the scripts that build them.
