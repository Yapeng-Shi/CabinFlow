#!/usr/bin/env bash
set -euo pipefail
[[ $# == 0 ]] || { echo "用法：bash $0" >&2; exit 2; }
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)
model_dir="$repo/runtime/build/models/text2vec-base-chinese/hf"
manifest="$repo/agent/models/rag-model.sha256"
revision=183bb99aa7af74355fb58d16edf8c13ae7c5433e
while read -r digest relative; do
    target="$model_dir/$relative"
    mkdir -p "$(dirname "$target")"
    if [[ ! -f $target ]]; then
        curl --fail --location --silent --show-error --connect-timeout 15 --max-time 900 \
            --output "$target.part" \
            "https://huggingface.co/shibing624/text2vec-base-chinese/resolve/$revision/$relative?download=true"
        printf '%s  %s\n' "$digest" "$target.part" | sha256sum --check
        mv -- "$target.part" "$target"
    fi
done < "$manifest"
(cd "$model_dir" && sha256sum --check "$manifest")
# 仅 RAG 使用这个固定环境；不改全局 Python，不替换 LLM 或引入备用后端。
venv="$repo/runtime/build/inference/rag-python/venv"
[[ -d $venv ]] || python3.12 -m venv "$venv"
"$venv/bin/pip" install 'torch==2.7.1+cpu' --index-url https://download.pytorch.org/whl/cpu
"$venv/bin/pip" install 'transformers==4.51.3' 'numpy==2.2.6'
"$venv/bin/pip" freeze
echo '资源准备完成；检索对照与端到端验收需要单独执行。'
