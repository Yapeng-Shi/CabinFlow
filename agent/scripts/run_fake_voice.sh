#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${repo_dir}/runtime"
cmake --preset linux-debug
cmake --build --preset linux-debug --target fake_voice_demo -j2
demo="${repo_dir}/runtime/build/linux-debug/agent-fake-voice-demo/fake_voice_demo"
input="${repo_dir}/runtime/build/linux-debug/fake_voice_input.wav"
output="${repo_dir}/runtime/build/linux-debug/fake_voice_answer.wav"
"${demo}" --write-fixture "${input}"
"${demo}" "${input}" "${output}"
