# Initial monorepo migration

This repository was initialized on 2026-09-19 from two local working trees:

- `CabinFlow-Runtime` was imported under `runtime/`.
- `CabinFlow-Agent` was imported under `agent/`.

The import intentionally excludes nested Git metadata, generated `build/` and
`install/` directories, local IDE settings, and Python cache directories. It
retains source code, project documentation, model metadata, and the current
working-tree edits so the new repository represents the active development
state.

The migration starts a new repository history. It does not rewrite or alter the
original repositories.

## Deferred LFS model artifacts

Three files in the Agent working tree are Git LFS pointer files, but their LFS
objects are not present in the local clone. They are intentionally excluded
from this initial public commit so a fresh clone does not contain unresolved
model references:

- `agent/tts/models/single_speaker_fast.bin`
- `agent/voice/models/sherpa-onnx-streaming-zipformer-small-bilingual-zh-en-2023-02-16/encoder-epoch-99-avg-1.onnx`
- `agent/automotive_edge_rag/models/model.safetensors`

When the original LFS objects or documented download sources are available,
restore them through Git LFS or a reproducible model-download script. They are
not required for the runtime build-sanity test.
