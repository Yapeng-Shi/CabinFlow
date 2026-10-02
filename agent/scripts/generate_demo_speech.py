#!/usr/bin/env python3
"""Generate registered synthetic Demo inputs, then compare real ASR transcripts."""

import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
import unicodedata
import wave

import run_model_smoke as smoke


INPUTS = smoke.REPO / "agent/models/demo-speech-inputs.json"


def normalize_transcript(text):
    # 只消除朗读中没有字面声音的标点与空白；数字/词语错误仍必须暴露。
    return "".join(char for char in text
                   if not char.isspace() and not unicodedata.category(char).startswith("P"))


def validate_cases(inputs):
    seen = set()
    for case in inputs["cases"]:
        case_id = case["id"]
        if not re.fullmatch(r"[a-z][a-z0-9_]*", case_id) or case_id in seen:
            raise ValueError("fixture IDs must be unique, safe directory names")
        if not case["text"].strip() or not case["reference_text"].strip():
            raise ValueError("fixture text and reference are required")
        if normalize_transcript(case["text"]) != normalize_transcript(case["reference_text"]):
            raise ValueError("reference must match the registered synthesis text")
        seen.add(case_id)
    if not seen:
        raise ValueError("at least one registered fixture is required")


def generate(output, report):
    inputs = json.loads(INPUTS.read_text(encoding="utf-8"))
    validate_cases(inputs)
    smoke.verify_manifest(smoke.MODELS, smoke.REPO / "agent/models/x86-models.sha256")
    smoke.verify_manifest(smoke.INFERENCE, smoke.REPO / "agent/models/sherpa-runtime.sha256")
    smoke.verify_manifest(smoke.REPO, smoke.REPO / "agent/models/asr-int8.sha256")
    report["registered_inputs"] = inputs
    report["registered_inputs_sha256"] = smoke.digest(INPUTS)
    report["model_license_sha256"] = smoke.digest(smoke.REPO / inputs["model_license_file"])
    for case in inputs["cases"]:
        case_dir = output / case["id"]
        case_dir.mkdir()
        record = {"stages": {}, "reference_text": case["reference_text"],
                  "human_listening": "not_verified"}
        report["cases"][case["id"]] = record
        config = {"threads": inputs["threads"], "tts": {
            "text": case["text"], "speaker_id": inputs["speaker_id"], "speed": inputs["speed"]}}
        smoke.smoke_tts(config, smoke.SHERPA / "bin/sherpa-onnx-offline-tts",
                        str(inputs["threads"]), case_dir, record)
        record["audio_path"] = str(case_dir / "tts.wav")

    all_match = True
    for case in inputs["cases"]:
        record = report["cases"][case["id"]]
        smoke.smoke_asr(inputs, Path(record["audio_path"]), smoke.SHERPA / "bin/sherpa-onnx",
                        str(inputs["threads"]), output / case["id"], record)
        decoded = record["stages"]["asr"]["text"]
        matched = normalize_transcript(decoded) == normalize_transcript(case["reference_text"])
        record["stages"]["asr"].update(
            semantic_acceptance="reference_matched" if matched else "reference_mismatch",
            status="passed" if matched else "failed")
        all_match = all_match and matched
        print(f"{case['id']}: reference_match={matched}, decoded={decoded!r}", flush=True)
    report["status"] = "passed" if all_match else "failed"
    if not all_match:
        report["error"] = "ASR differs from registered synthetic reference; no reference was rewritten"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=False)
    report = {"status": "running", "cases": {}, "runtime_connected": False,
              "source_type": "synthetic", "human_listening": "not_verified",
              "timing_boundary": "independent process start to exit, including loading"}
    try:
        generate(args.output_dir, report)
    except (ValueError, OSError, KeyError, TypeError, subprocess.TimeoutExpired, wave.Error) as error:
        report["status"] = "failed"
        report["error"] = str(error)
        print(f"fixture preparation failed: {error}", file=sys.stderr)
    finally:
        (args.output_dir / "report.json").write_text(
            json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"{report['status']}: {args.output_dir / 'report.json'}")
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__":
    sys.exit(main())
