#!/usr/bin/env python3
"""Offline, independent model smoke tests; never a Runtime or fake fallback."""

import argparse
import array
import hashlib
import json
import platform
from pathlib import Path
import subprocess
import sys
import time
import wave


REPO = Path(__file__).resolve().parents[2]
ASR_DIR = REPO / "agent/voice/models/sherpa-onnx-streaming-zipformer-small-bilingual-zh-en-2023-02-16"
MODELS = REPO / "runtime/build/models"
INFERENCE = REPO / "runtime/build/inference"
SHERPA = INFERENCE / "sherpa-onnx-v1.11.3-linux-x64-shared"


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def verify_file(path, expected):
    if not path.is_file():
        raise ValueError(f"required file missing: {path}")
    with path.open("rb") as stream:
        if stream.read(128).startswith(b"version https://git-lfs.github.com/spec/v1"):
            raise ValueError(f"Git LFS pointer is not a model: {path}")
    if digest(path) != expected:
        raise ValueError(f"SHA-256 mismatch: {path}")


def verify_manifest(base, manifest):
    for line in manifest.read_text(encoding="utf-8").splitlines():
        expected, relative = line.split(maxsplit=1)
        verify_file(base / relative, expected)


def inspect_wav(path):
    with wave.open(str(path), "rb") as wav:
        if wav.getcomptype() != "NONE" or wav.getnchannels() != 1 or wav.getsampwidth() != 2:
            raise ValueError(f"expected mono PCM16 WAV: {path}")
        frames = wav.getnframes()
        raw = wav.readframes(frames)
        if frames == 0 or len(raw) != frames * 2:
            raise ValueError(f"empty or truncated WAV: {path}")
        samples = array.array("h", raw)
        if sys.byteorder != "little":
            samples.byteswap()
        if not any(samples):
            raise ValueError(f"silent WAV is not a speech smoke result: {path}")
        return {"sample_rate": wav.getframerate(), "channels": wav.getnchannels(),
                "sample_width": wav.getsampwidth(), "frames": frames,
                "duration_seconds": frames / wav.getframerate(), "sha256": digest(path)}


def run_stage(name, command, output, report):
    stdout_path, stderr_path = output / f"{name}.stdout.txt", output / f"{name}.stderr.txt"
    record = {"command": [str(arg) for arg in command], "status": "running"}
    report["stages"][name] = record
    started = time.monotonic_ns()
    # 每次启动真实子进程并保存本次证据；超时是失败，不会重试或读取历史结果。
    try:
        with stdout_path.open("w", encoding="utf-8") as stdout, stderr_path.open("w", encoding="utf-8") as stderr:
            completed = subprocess.run(record["command"], stdout=stdout, stderr=stderr,
                                       timeout=300, check=False)
        record["exit_code"] = completed.returncode
        if completed.returncode != 0:
            raise ValueError(f"{name} failed with exit {completed.returncode}; see {stderr_path}")
    except (OSError, subprocess.TimeoutExpired, ValueError):
        record["status"] = "failed"
        raise
    finally:
        record["process_elapsed_ns"] = time.monotonic_ns() - started
    record["status"] = "process_passed"
    return stdout_path.read_text(encoding="utf-8"), stderr_path.read_text(encoding="utf-8")


def parse_llm_output(stdout):
    # b6500 CLI 把结束提示写到 stdout；只去掉已知终止提示，不改写模型答案。
    marker = " [end of text]"
    if not stdout.rstrip().endswith(marker):
        raise ValueError("LLM did not terminate with the pinned CLI end marker")
    return stdout.rstrip()[:-len(marker)].strip()


def smoke(output, report, component="all"):
    inputs = json.loads((REPO / "agent/models/model-smoke-inputs.json").read_text(encoding="utf-8"))
    verify_manifest(REPO, REPO / "agent/models/asr-int8.sha256")
    verify_manifest(MODELS, REPO / "agent/models/x86-models.sha256")
    verify_manifest(INFERENCE, REPO / "agent/models/x86-inference.sha256")
    verify_manifest(INFERENCE, REPO / "agent/models/sherpa-runtime.sha256")
    verify_manifest(INFERENCE, INFERENCE / "llama-cli.sha256")
    audio = REPO / inputs["asr"]["path"]
    verify_file(audio, inputs["asr"]["sha256"])
    audio_info = inspect_wav(audio)
    for key in ("sample_rate", "channels", "sample_width", "frames"):
        if audio_info[key] != inputs["asr"][key]:
            raise ValueError(f"ASR fixture {key} differs from registered input")
    report["inputs"] = inputs
    report["asr_audio"] = audio_info
    threads = str(inputs["threads"])
    asr = SHERPA / "bin/sherpa-onnx"
    tts = SHERPA / "bin/sherpa-onnx-offline-tts"
    llm = INFERENCE / "llama-build/bin/llama-cli"
    report["executables_sha256"] = {str(path): digest(path) for path in (asr, llm, tts)}

    if component in ("all", "asr"):
        smoke_asr(inputs, audio, asr, threads, output, report)
    if component in ("all", "llm"):
        smoke_llm(inputs, llm, threads, output, report)
    if component in ("all", "tts"):
        smoke_tts(inputs, tts, threads, output, report)


def smoke_asr(inputs, audio, asr, threads, output, report):

    _, stderr = run_stage("asr", [asr, f"--tokens={ASR_DIR / 'tokens.txt'}",
        f"--encoder={ASR_DIR / 'encoder-epoch-99-avg-1.int8.onnx'}",
        f"--decoder={ASR_DIR / 'decoder-epoch-99-avg-1.int8.onnx'}",
        f"--joiner={ASR_DIR / 'joiner-epoch-99-avg-1.int8.onnx'}", "--provider=cpu",
        f"--num-threads={threads}", "--decoding-method=greedy_search", audio], output, report)
    # 固定版本的 CLI 在 stderr 输出单行 JSON；这不是 Runtime 协议解析器。
    results = [json.loads(line) for line in stderr.splitlines() if line.startswith("{")]
    if len(results) != 1 or not results[0].get("text", "").strip():
        raise ValueError("ASR did not produce the required non-empty decoded text")
    report["stages"]["asr"].update(status="passed", text=results[0]["text"], semantic_acceptance="not_evaluated")


def smoke_llm(inputs, llm, threads, output, report):
    config = inputs["llm"]
    stdout, _ = run_stage("llm", [llm, "-m", MODELS / "qwen3-0.6b/Qwen3-0.6B-Q8_0.gguf",
        "-p", config["prompt"], "-no-cnv", "--no-display-prompt", "--simple-io",
        "--seed", str(config["seed"]), "--temp", str(config["temperature"]),
        "--top-p", str(config["top_p"]), "--top-k", str(config["top_k"]),
        "--min-p", str(config["min_p"]), "--presence-penalty", str(config["presence_penalty"]),
        "-n", str(config["max_tokens"]), "-c", str(config["context_size"]),
        "-t", threads, "-ngl", "0"], output, report)
    answer = parse_llm_output(stdout)
    if answer != config["expected_text"]:
        raise ValueError(f"LLM answer differs from registered expectation: {stdout!r}")
    report["stages"]["llm"].update(status="passed", text=answer)


def smoke_tts(inputs, tts, threads, output, report):
    model = MODELS / "vits-melo-tts-zh_en"
    answer = output / "tts.wav"
    run_stage("tts", [tts, f"--vits-model={model / 'model.onnx'}",
        f"--vits-tokens={model / 'tokens.txt'}", f"--vits-lexicon={model / 'lexicon.txt'}",
        f"--vits-dict-dir={model / 'dict'}", f"--tts-rule-fsts={model / 'date.fst'},{model / 'number.fst'},{model / 'new_heteronym.fst'}",
        "--provider=cpu", f"--num-threads={threads}", f"--sid={inputs['tts']['speaker_id']}",
        f"--vits-length-scale={inputs['tts']['speed']}", f"--output-filename={answer}",
        inputs["tts"]["text"]], output, report)
    report["stages"]["tts"].update(status="passed", audio=inspect_wav(answer), human_listening="not_verified")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--component", choices=("all", "asr", "llm", "tts"), default="all")
    args = parser.parse_args()
    # 不允许复用输出目录，避免旧 WAV 或报告被误认为这次执行成功。
    args.output_dir.mkdir(parents=True, exist_ok=False)
    report = {"status": "running", "date_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
              "environment": {"platform": platform.platform(), "machine": platform.machine(),
                              "python": platform.python_version()}, "stages": {},
              "timing_boundary": "independent process start to exit, including model loading",
              "dependencies": {"sherpa": "1.11.3 upstream CPU shared release, not locally source-built",
                               "llama_cpp": "b6500/a7a98e0fffed794396b3fbad4dcdbbc184963645 local Release CPU build",
                               "prepare_script_sha256": digest(REPO / "agent/scripts/prepare_x86_inference.sh")},
              "runtime_connected": False, "component": args.component}
    try:
        smoke(args.output_dir, report, args.component)
        report["status"] = "passed"
    except (ValueError, OSError, KeyError, TypeError, subprocess.TimeoutExpired, wave.Error) as error:
        report["status"] = "failed"
        report["error"] = str(error)
        for stage in report["stages"].values():
            if stage["status"] == "process_passed":
                stage["status"] = "failed"
        print(f"model smoke failed: {error}", file=sys.stderr)
    finally:
        (args.output_dir / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"{report['status']}: {args.output_dir / 'report.json'}")
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__":
    sys.exit(main())
