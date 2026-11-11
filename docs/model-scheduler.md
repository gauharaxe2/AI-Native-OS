# Model scheduler

`aios-modeld` is the phase-3 local scheduler. `ai-os-init` starts it after `aios-auditd`. It looks for a `*.gguf` file in `/var/lib/aios/models`, starts `llama-server` on `127.0.0.1:8081`, and answers a Unix socket at `/run/aios/model.sock`.

There is no cloud client. A cloud request is only an audit record.

## Weights

The git tree does not contain a model. `scripts/fetch-model.sh` downloads one and packs `models/model.img`:

| Piece | What it is | License |
| --- | --- | --- |
| Weights | [SmolLM2-135M-Instruct](https://huggingface.co/HuggingFaceTB/SmolLM2-135M-Instruct) | Apache-2.0 |
| File on the disk | bartowski's Q4_0 GGUF of those weights, about 88 MB | Same Apache-2.0 terms |
| Runtime | llama.cpp tag `b6000`, CPU only, `llama-cli` and `llama-server` | MIT |

`scripts/run-qemu.sh` attaches `models/model.img` as a second virtio disk when the file exists. Init mounts it on `/var/lib/aios/models`. The smoke test gives the guest 3 GB of RAM and 2 CPUs. Inference is still emulated aarch64, so prompts are capped (`n_predict` 24 unless `AIOS_MODEL_PREDICT` is set).

## Calls

`aios ask "..."` asks the audit daemon for `model.local` as actor `user`. The default policy allows that risk level and appends a record whose result is `permitted` and whose undo is `irreversible`. Only after that record is written does the scheduler format a short SmolLM2 chat prompt and call `llama-server`.

`aios request model.cloud <target>` is `ask`. The daemon records `pending` and waits. Nothing is sent off the machine. Approve or deny it like any other pending action; approving still does not contact a cloud.

`aios model status` prints `state:` (`missing`, `loading`, `ready`, or `failed`) and `model:`. `aios status` shows the scheduler as running once the socket answers, and it shows the GGUF file name on the `model:` line.

## Build

```bash
scripts/fetch-model.sh
scripts/build.sh
scripts/run-qemu.sh --smoke
```

`llama-cli` is installed for direct use. The scheduler itself talks to `llama-server`. Both are built for `armv8-a` so they run on QEMU's Cortex-A53 without later ARM instructions.
