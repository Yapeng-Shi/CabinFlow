#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)
models="$repo/runtime/build/models/wake"
bundle=sherpa-onnx-kws-zipformer-wenetspeech-3.3M-2024-01-01
mkdir -p "$models"
if [[ ! -f $models/kws.tar.bz2 ]]; then
    curl --fail --location --silent --show-error --connect-timeout 15 --max-time 180 \
        --output "$models/kws.tar.bz2.part" \
        "https://github.com/k2-fsa/sherpa-onnx/releases/download/kws-models/$bundle.tar.bz2"
    mv -- "$models/kws.tar.bz2.part" "$models/kws.tar.bz2"
fi
printf '%s  %s\n' b2f7c89690dc8ce4c6ed6afeab7cd800c36ad1421fb6b6302b4a4b194cf7f35f "$models/kws.tar.bz2" | sha256sum --check
# 先验证固定归档，再解包；已有模型不覆盖，最终清单校验失败就停止。
if [[ ! -d $models/$bundle ]]; then tar xf "$models/kws.tar.bz2" -C "$models"; fi
if [[ ! -f $models/silero_vad_v5.onnx ]]; then
    curl --fail --location --silent --show-error --connect-timeout 15 --max-time 60 \
        --output "$models/silero_vad_v5.onnx.part" \
        https://github.com/snakers4/silero-vad/raw/refs/tags/v5.0/files/silero_vad.onnx
    mv -- "$models/silero_vad_v5.onnx.part" "$models/silero_vad_v5.onnx"
fi
(cd "$models" && sha256sum --check "$repo/agent/models/wake-models.sha256")
echo "Wake models verified: $models (microphone remains opt-in)"
