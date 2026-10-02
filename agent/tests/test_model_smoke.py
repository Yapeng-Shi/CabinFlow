"""Deterministic failure tests for the independent smoke runner, no models needed."""

import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import wave


SPEC = importlib.util.spec_from_file_location("model_smoke", Path(__file__).resolve().parents[1] / "scripts/run_model_smoke.py")
smoke = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(smoke)


class ModelSmokeTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)

    def write_wav(self, data):
        path = self.directory / "fixture.wav"
        with wave.open(str(path), "wb") as wav:
            wav.setnchannels(1)
            wav.setsampwidth(2)
            wav.setframerate(16000)
            wav.writeframes(data)
        return path

    def test_missing_file_is_explicit(self):
        with self.assertRaisesRegex(ValueError, "required file missing"):
            smoke.verify_file(self.directory / "missing", "0" * 64)

    def test_lfs_pointer_is_not_a_model(self):
        path = self.directory / "model.onnx"
        path.write_bytes(b"version https://git-lfs.github.com/spec/v1\n")
        with self.assertRaisesRegex(ValueError, "Git LFS pointer"):
            smoke.verify_file(path, hashlib.sha256(path.read_bytes()).hexdigest())

    def test_bad_hash_is_explicit(self):
        path = self.directory / "model"
        path.write_bytes(b"bad model")
        with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
            smoke.verify_file(path, "0" * 64)

    def test_silence_is_not_a_real_speech_result(self):
        with self.assertRaisesRegex(ValueError, "silent WAV"):
            smoke.inspect_wav(self.write_wav(b"\x00\x00" * 4))

    def test_truncated_wav_is_rejected(self):
        path = self.write_wav(b"\x01\x00" * 4)
        path.write_bytes(path.read_bytes()[:-2])
        with self.assertRaisesRegex(ValueError, "truncated WAV"):
            smoke.inspect_wav(path)

    def test_valid_pcm_has_expected_metadata(self):
        result = smoke.inspect_wav(self.write_wav(b"\x01\x00" * 4))
        self.assertEqual(result["frames"], 4)
        self.assertEqual(result["sample_rate"], 16000)

    def test_process_failure_is_not_success(self):
        report = {"stages": {}}
        with self.assertRaisesRegex(ValueError, "exit 7"), patch.object(
                smoke.subprocess, "run", return_value=subprocess.CompletedProcess(["test"], 7)):
            smoke.run_stage("asr", ["test"], self.directory, report)
        self.assertEqual(report["stages"]["asr"]["status"], "failed")
        self.assertEqual(report["stages"]["asr"]["exit_code"], 7)

    def test_timeout_is_recorded_as_failure(self):
        report = {"stages": {}}
        with self.assertRaises(subprocess.TimeoutExpired), patch.object(
                smoke.subprocess, "run", side_effect=subprocess.TimeoutExpired(["test"], 300)):
            smoke.run_stage("tts", ["test"], self.directory, report)
        self.assertEqual(report["stages"]["tts"]["status"], "failed")

    def test_bad_config_records_failed_report(self):
        output = self.directory / "output"
        with patch.object(smoke.sys, "argv", ["test", "--output-dir", str(output)]), patch.object(
                smoke, "smoke", side_effect=KeyError("required config")):
            self.assertEqual(smoke.main(), 1)
        self.assertEqual(json.loads((output / "report.json").read_text())["status"], "failed")

    def test_existing_output_cannot_be_reused(self):
        with patch.object(smoke.sys, "argv", ["test", "--output-dir", str(self.directory)]):
            with self.assertRaises(FileExistsError):
                smoke.main()

    def test_cli_end_marker_is_not_model_text(self):
        self.assertEqual(smoke.parse_llm_output("2 [end of text]\n\n"), "2")

    def test_wrong_answer_remains_wrong(self):
        self.assertNotEqual(smoke.parse_llm_output("1 [end of text]\n\n"), "2")

    def test_unterminated_cli_output_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "end marker"):
            smoke.parse_llm_output("2")


if __name__ == "__main__":
    unittest.main()
