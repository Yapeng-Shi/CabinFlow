#!/usr/bin/env python3
"""Generate a separate synthetic KWS fixture set; never change the ASR references."""
import argparse
import json
import wave
from pathlib import Path
import run_model_smoke as smoke
from prepare_voice_demo_cases import convert

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=False)
    smoke.verify_manifest(smoke.MODELS, smoke.REPO / "agent/models/x86-models.sha256")
    report = {"source_type": "synthetic", "human_microphone": "not_verified", "cases": {}}
    for name, text in {"wake": "你好，小舱。", "command": "请打开空调。", "negative": "今天天气怎么样。"}.items():
        directory = args.output_dir / name
        directory.mkdir()
        entry = {"stages": {}, "text": text}
        smoke.smoke_tts({"tts": {"text": text, "speaker_id": 0, "speed": 1.5}},
                        smoke.SHERPA / "bin/sherpa-onnx-offline-tts", "1", directory, entry)
        target = args.output_dir / (name + ".wav")
        # 测试语料准备明确转码；生产采音仍必须提供固定16k mono PCM16，不猜格式。
        converted = directory / "mono.wav"
        convert(directory / "tts.wav", converted)
        with wave.open(str(converted), "rb") as source:
            params = source.getparams()
            samples = source.readframes(source.getnframes())
        # 固定测试输入显式包含说完后的安静，不用离线InputFinished替代真实流的尾部。
        with wave.open(str(target), "wb") as wav:
            wav.setparams(params)
            wav.writeframes(samples + bytes(32000))
        entry["wav_sha256"] = smoke.digest(target)
        report["cases"][name] = entry
    (args.output_dir / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(args.output_dir)

if __name__ == "__main__":
    main()
