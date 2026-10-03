#!/usr/bin/env python3
"""Run the fixed 5x3 CLI integration cases; not a Qt or human-listening acceptance."""

import argparse
import json
import os
from pathlib import Path
import platform
import re
import signal
import subprocess
import time
import wave

import generate_demo_speech as speech
import run_model_smoke as smoke


def parse_result(path):
    text = path.read_text(encoding="utf-8")
    # 读取本仓库唯一 CLI 报告格式；保留原文件，不改回答、参考或失败含义。
    identity, content = text.split("\ntranscript=", 1)
    transcript, content = content.split("\nanswer=", 1)
    answer, tail = content.rsplit("\nerror=", 1)
    error, _ = tail.split("\nsemantic_quality=", 1)
    fields = dict(line.split("=", 1) for line in identity.splitlines())
    receipt = re.search(r"\nvehicle_simulated=([01])\nclimate_on=([01])\naction_applied=([01])\nleft_front_window_open=([01])\n$", text)
    if receipt is None and not text.endswith("\nvehicle_state=unknown\n"):
        raise ValueError("vehicle receipt missing required left_front_window_open or malformed")
    return dict(fields, transcript=transcript, answer=answer, error=error,
                vehicle=None if receipt is None else dict(zip(
                    ("simulated", "climate_on", "action_applied", "left_front_window_open"), [value == "1" for value in receipt.groups()])))


def parse_timings(stdout, request_id):
    records = []
    for line in stdout.splitlines():
        if " event=backend_call_completed " not in line:
            continue
        match = re.search(r" node=(\S+).* work_id=(\S+) message_id=(\S+) status=(\S+) detail=duration_ns=(\d+) request_message_id=(\S+)$", line)
        if not match or match.group(6) != request_id:
            raise ValueError("backend timing identity differs from CLI root request")
        records.append({"node": match.group(1), "work_id": match.group(2),
                        "message_id": match.group(3), "backend_error": match.group(4),
                        "duration_ns": int(match.group(5)), "request_message_id": match.group(6)})
    return records


def resources(path):
    text = path.read_text(encoding="utf-8")
    values = {}
    for label, key, converter in (
        ("User time (seconds)", "user_cpu_seconds", float),
        ("System time (seconds)", "system_cpu_seconds", float),
        ("Maximum resident set size (kbytes)", "max_rss_kib", int),
    ):
        match = re.search(re.escape(label) + r": ([\d.]+)", text)
        if not match:
            raise ValueError("GNU time resource field missing: " + label)
        values[key] = converter(match.group(1))
    return values


def wait_owned_process(process, record, timeout_seconds):
    try:
        return process.wait(timeout=timeout_seconds)
    except BaseException as error:
        # 子进程是本次新建的独立 session；Ctrl+C/等待异常也必须回收，不能留下后台模型。
        record["error"] = ("interrupted" if isinstance(error, KeyboardInterrupt) else str(error))
        record["forced_process_cleanup"] = True
        record["graceful_cancellation"] = False
        # group 可能仍有子孙进程，不能用 GNU time 直接子进程的退出状态推断 group 已空。
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass  # group 已退出的真实竞态；仍 wait 回收已拥有的直接子进程。
        process.wait()
        raise


def run(args, report):
    index = json.loads(args.cases.read_text(encoding="utf-8"))
    inputs = json.loads(speech.INPUTS.read_text(encoding="utf-8"))
    if index["registered_inputs_sha256"] != smoke.digest(speech.INPUTS) or len(index["cases"]) != 5:
        raise ValueError("fixed fixture revision/count differs from registered cases")
    for case, registered in zip(index["cases"], inputs["cases"]):
        if any(case[key] != registered[key] for key in ("id", "text", "reference_text", "expected_result")):
            raise ValueError("registered reference or expectation was changed")
        smoke.verify_file(Path(case["wav_path"]), case["input_audio"]["sha256"])
        if case["input_audio"]["sample_rate"] != 16000:
            raise ValueError("fixture has not been explicitly converted to 16 kHz")
    report["fixture_index"] = str(args.cases.resolve())
    report["fixture_index_sha256"] = smoke.digest(args.cases)
    report["runner_sha256"] = smoke.digest(Path(__file__))
    report["binary_sha256"] = smoke.digest(args.binary)
    report["registered_versions"] = {"sherpa": "1.11.3", "onnxruntime": "1.17.1",
                                     "llama_cpp": "b6500", "llm": "Qwen3-0.6B Q8_0 non-thinking",
                                     "model_threads": 2, "context": 2048, "max_new_tokens": 128}
    report["measurement_conditions"] = "Shared WSL development host; background contract tests may overlap; raw observations, not an uncontended benchmark."
    report["model_paths"] = [str(args.asr_model), str(args.llm_model), str(args.tts_model)]
    report["llm_sha256"] = smoke.digest(args.llm_model)
    report["host"] = {"platform": platform.platform(), "python": platform.python_version(),
                      "cpuinfo": Path("/proc/cpuinfo").read_text(encoding="utf-8")}
    report["timing_boundaries"] = {
        "process_elapsed_ns": "GNU time + CLI start to exit, includes loading, setup, queues, inference, WAV/report writes and shutdown",
        "backend_duration_ns": "backend call only; excludes loading, queue, measurement log and serialization; exceptions have no completion timing",
        "resources": "GNU time user/system CPU and max RSS per CLI process; not steady-state server or GUI metrics"}
    (args.output_dir / "cases.json").write_text(json.dumps(index, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    for case in index["cases"]:
        for repetition in range(1, 4):
            case_dir = args.output_dir / (case["id"] + "-" + str(repetition))
            case_dir.mkdir()
            command = ["/usr/bin/time", "-v", "-o", str(case_dir / "resources.txt"),
                       str(args.binary.resolve()), "--wav", case["wav_path"], str(case_dir / "result"),
                       str(args.asr_model.resolve()), str(args.llm_model.resolve()), str(args.tts_model.resolve())]
            record = {"case_id": case["id"], "repetition": repetition, "command": command,
                      "reference_text": case["reference_text"], "expected_result": case["expected_result"],
                      "input_audio": case["input_audio"], "source_type": "synthetic",
                      "human_listening": "not_verified", "answer_semantic_acceptance": "not_verified"}
            report["samples"].append(record)
            started = time.monotonic_ns()
            with (case_dir / "stdout.txt").open("w", encoding="utf-8") as stdout, (case_dir / "stderr.txt").open("w", encoding="utf-8") as stderr:
                process = subprocess.Popen(command, stdout=stdout, stderr=stderr,
                                           env=dict(os.environ, LC_ALL="C"), start_new_session=True)
                try:
                    record["exit_code"] = wait_owned_process(process, record, 180)
                finally:
                    record["process_elapsed_ns"] = time.monotonic_ns() - started
            record["resources"] = resources(case_dir / "resources.txt")
            record["execution_completed"] = record["exit_code"] == 0
            result_path = case_dir / "result/result.txt"
            if not result_path.is_file():
                raise ValueError("CLI did not produce a task report; see stderr, no historical output accepted")
            result = parse_result(result_path)
            record["result"] = result
            record["asr_reference_matched"] = speech.normalize_transcript(result["transcript"]) == speech.normalize_transcript(case["reference_text"])
            record["backend_timings"] = parse_timings((case_dir / "stdout.txt").read_text(encoding="utf-8"), result["request_message_id"])
            receipt = result["vehicle"]
            expected_action = case["id"] in ("ac_on", "ac_off")
            expected_on = case["id"] == "ac_on"
            record["vehicle_expectation_met"] = bool(receipt and receipt["simulated"] and
                receipt["action_applied"] == expected_action and receipt["climate_on"] == expected_on)
            if record["execution_completed"]:
                record["answer_audio"] = smoke.inspect_wav(case_dir / "result/answer.wav")
            print(f"{case['id']} #{repetition}: execution={record['execution_completed']} reference={record['asr_reference_matched']} vehicle={record['vehicle_expectation_met']}", flush=True)
            (args.output_dir / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    report["observed_contracts_passed"] = all(sample["execution_completed"] and sample["asr_reference_matched"] and sample["vehicle_expectation_met"] for sample in report["samples"])
    report["status"] = "awaiting_human_quality_acceptance" if report["observed_contracts_passed"] else "failed_observed_contracts"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("cases", "binary", "output-dir", "asr-model", "llm-model", "tts-model"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=False)
    report = {"status": "running", "samples": [], "concurrency": 1, "planned_sample_count": 15,
              "entry": "CLI integration only, not Qt/TCP acceptance", "human_listening": "not_verified"}
    try:
        run(args, report)
    except KeyboardInterrupt:
        report.update(status="interrupted", error="operator interruption; forced child cleanup is not cooperative cancellation")
        raise
    except (OSError, ValueError, KeyError, TypeError, subprocess.TimeoutExpired, wave.Error) as error:
        report.update(status="failed_runner", error=str(error))
        raise
    finally:
        (args.output_dir / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"{report['status']}: {args.output_dir / 'report.json'}")
    return 1 if report["status"] == "failed_observed_contracts" else 0


if __name__ == "__main__":
    raise SystemExit(main())
