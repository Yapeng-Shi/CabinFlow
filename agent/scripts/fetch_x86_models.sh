#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
model_dir="${repo_dir}/runtime/build/models"
manifest="${repo_dir}/agent/models/x86-models.sha256"
asr_manifest="${repo_dir}/agent/models/asr-int8.sha256"
tts_revision="a0d5c6a264c0ef92d70d8661d8cc502d79627cd6"
llm_revision="23749fefcc72300e3a2ad315e1317431b06b590a"

fetch() {
    local relative="$1"
    local url="$2"
    local target="${model_dir}/${relative}"
    mkdir -p "$(dirname "${target}")"
    if [[ -f "${target}" ]]; then
        return
    fi
    curl --fail --location --show-error --silent \
        --connect-timeout 15 --max-time 900 \
        --output "${target}.part" "${url}"
    mv -- "${target}.part" "${target}"
}

# manifest 同时是唯一文件清单和完整性契约，避免下载列表与哈希列表漂移。
while read -r digest relative; do
    [[ -n "${digest}" ]] || continue
    case "${relative}" in
        vits-melo-tts-zh_en/*)
            path="${relative#vits-melo-tts-zh_en/}"
            base="https://huggingface.co/csukuangfj/vits-melo-tts-zh_en/resolve/${tts_revision}"
            ;;
        qwen3-0.6b/*)
            path="${relative#qwen3-0.6b/}"
            base="https://huggingface.co/Qwen/Qwen3-0.6B-GGUF/resolve/${llm_revision}"
            ;;
        *)
            printf 'unknown model path in manifest: %s\n' "${relative}" >&2
            exit 1
            ;;
    esac
    if [[ -z "${path}" || "${path}" == *..* ]]; then
        printf 'invalid model path in manifest: %s\n' "${relative}" >&2
        exit 1
    fi
    fetch "${relative}" "${base}/${path}?download=true"
done < "${manifest}"

# 缓存只在全部 SHA-256 校验通过后才被视为可用；不存在隐式重试或替代源。
(cd "${repo_dir}" && sha256sum --check "${asr_manifest}")
(cd "${model_dir}" && sha256sum --check "${manifest}")
printf 'x86 model files verified under %s\n' "${model_dir}"
