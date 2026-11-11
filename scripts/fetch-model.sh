#!/usr/bin/env bash
# Download the phase-3 test model and pack it into models/model.img.
# The weights are not committed. SmolLM2-135M-Instruct is Apache-2.0.
# This file is bartowski's Q4_0 GGUF conversion of those weights.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
DEST="$ROOT/models"
FILE="SmolLM2-135M-Instruct-Q4_0.gguf"
URL="https://huggingface.co/bartowski/SmolLM2-135M-Instruct-GGUF/resolve/main/${FILE}"
STAGE="$DEST/image-root"
IMG="$DEST/model.img"

mkdir -p "$DEST" "$STAGE"
if [[ ! -f "$DEST/$FILE" ]]; then
    echo "fetch-model: downloading ${FILE}"
    wget -O "$DEST/$FILE.partial" --user-agent="AI-Native-OS-fetch" "$URL"
    mv "$DEST/$FILE.partial" "$DEST/$FILE"
fi

if command -v mkfs.ext4 >/dev/null 2>&1; then
    MKFS=$(command -v mkfs.ext4)
elif [[ -x "$ROOT/output/host/sbin/mkfs.ext4" ]]; then
    MKFS="$ROOT/output/host/sbin/mkfs.ext4"
else
    echo "fetch-model: mkfs.ext4 not found" >&2
    exit 1
fi

cp -f "$DEST/$FILE" "$STAGE/$FILE"
cat > "$STAGE/MODEL-LICENSE.txt" <<'EOF'
SmolLM2-135M-Instruct
Copyright Hugging Face
License: Apache-2.0
https://huggingface.co/HuggingFaceTB/SmolLM2-135M-Instruct

This disk holds a Q4_0 GGUF conversion published by bartowski:
https://huggingface.co/bartowski/SmolLM2-135M-Instruct-GGUF
The conversion does not change the Apache-2.0 terms of the weights.
EOF

rm -f "$IMG"
"$MKFS" -F -L aios-models -d "$STAGE" "$IMG" 160M
echo "fetch-model: wrote $IMG"
ls -lh "$IMG" "$DEST/$FILE"
