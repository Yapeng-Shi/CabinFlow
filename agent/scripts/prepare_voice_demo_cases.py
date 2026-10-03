#!/usr/bin/env python3
"""Freeze the approved synthetic inputs as explicit 16 kHz Demo fixtures (Python 3.12)."""

import argparse
import audioop
import json
from pathlib import Path
import wave

import generate_demo_speech as speech
import run_model_smoke as smoke


def convert(source, target):
    metadata = smoke.inspect_wav(source)
    if metadata["sample_rate"] != 44100:
        raise ValueError("registered source must be 44.1 kHz mono PCM16; no format guessing")
    with wave.open(str(source), "rb") as wav:
        raw = wav.readframes(wav.getnframes())
    # 仅在验收准备阶段显式转换；生产 ASR 仍拒绝不支持的采样率，不自动转码。
    converted, _ = audioop.ratecv(raw, 2, 1, 44100, 16000, None)
    with wave.open(str(target), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(16000)
        wav.writeframes(converted)
    return metadata, smoke.inspect_wav(target)


def prepare(source_dir, output_dir):
    inputs = json.loads(speech.INPUTS.read_text(encoding="utf-8"))
    speech.validate_cases(inputs)
    source_report = json.loads((source_dir / "report.json").read_text(encoding="utf-8"))
    if source_report["registered_inputs_sha256"] != smoke.digest(speech.INPUTS):
        raise ValueError("source synthesis report is not the registered input revision")
    license_path = smoke.REPO / inputs["model_license_file"]
    if source_report["model_license_sha256"] != smoke.digest(license_path):
        raise ValueError("synthetic speech license evidence changed")
    output_dir.mkdir(parents=True, exist_ok=False)
    index = {"source_type": "synthetic", "registered_inputs": str(speech.INPUTS),
             "registered_inputs_sha256": smoke.digest(speech.INPUTS),
             "source_report": str(source_dir / "report.json"),
             "source_report_sha256": smoke.digest(source_dir / "report.json"),
             "model_license_sha256": smoke.digest(license_path),
             "conversion": "Python 3.12 audioop.ratecv, 44100->16000, mono PCM16, offline only",
             "human_listening": "not_verified", "cases": []}
    for case in inputs["cases"]:
        source = source_dir / case["id"] / "tts.wav"
        expected = source_report["cases"][case["id"]]["stages"]["tts"]["audio"]["sha256"]
        smoke.verify_file(source, expected)
        target = output_dir / (case["id"] + ".wav")
        original, converted = convert(source, target)
        index["cases"].append(dict(case, wav_path=str(target.resolve()),
                                   source_audio=original, input_audio=converted))
    (output_dir / "cases.json").write_text(json.dumps(index, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(output_dir / "cases.json")
    return index


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    prepare(args.source_dir.resolve(), args.output_dir.resolve())


if __name__ == "__main__":
    main()
