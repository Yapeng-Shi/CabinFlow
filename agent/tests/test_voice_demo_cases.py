import json
from pathlib import Path
import sys
import subprocess
import signal
import tempfile
import unittest
from unittest import mock
import wave

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import prepare_voice_demo_cases as cases
import run_voice_demo_cases as runner
sys.path.pop(0)


class VoiceDemoCasesTest(unittest.TestCase):
    def test_conversion_changes_format_not_reference(self):
        with tempfile.TemporaryDirectory() as directory:
            source, target = Path(directory) / "source.wav", Path(directory) / "target.wav"
            with wave.open(str(source), "wb") as wav:
                wav.setnchannels(1)
                wav.setsampwidth(2)
                wav.setframerate(44100)
                wav.writeframes(b"\x01\x00" * 4410)
            original_hash = cases.smoke.digest(source)
            original, converted = cases.convert(source, target)
            self.assertEqual(original["sample_rate"], 44100)
            self.assertEqual(converted["sample_rate"], 16000)
            self.assertEqual(converted["channels"], 1)
            self.assertEqual(converted["sample_width"], 2)
            self.assertEqual(cases.smoke.digest(source), original_hash)
        inputs = json.loads(cases.speech.INPUTS.read_text(encoding="utf-8"))
        self.assertEqual(len(inputs["cases"]), 5)
        cases.speech.validate_cases(inputs)

    def test_unregistered_rate_is_not_guessed(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "source.wav"
            with wave.open(str(source), "wb") as wav:
                wav.setnchannels(1)
                wav.setsampwidth(2)
                wav.setframerate(8000)
                wav.writeframes(b"\x01\x00" * 800)
            with self.assertRaises(ValueError):
                cases.convert(source, Path(directory) / "target.wav")

    def test_stage_identity_and_boundary_are_explicit(self):
        line = "time_unix_ms=1 event=backend_call_completed node=asr.primary trace_id=t session_id=s work_id=w message_id=request status=kNone detail=duration_ns=123 request_message_id=request"
        timing = runner.parse_timings(line, "request")
        self.assertEqual(timing[0]["duration_ns"], 123)
        self.assertEqual(timing[0]["backend_error"], "kNone")
        with self.assertRaises(ValueError):
            runner.parse_timings(line, "other-request")
        with self.assertRaises(ValueError):
            runner.parse_timings(line.replace("duration_ns=123", "duration_ns=unknown"), "request")

    def test_multiline_cli_answer_is_not_rewritten(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "result.txt"
            path.write_text("status=0\ntrace_id=t\nsession_id=s\nwork_id=w\nrequest_message_id=request\ntranscript=原文\nanswer=第一行\nerror=回答中的文字\n第二行\nerror=\nsemantic_quality=not_automatically_verified\nhuman_listening=not_verified\nvehicle_simulated=1\nclimate_on=0\naction_applied=0\nleft_front_window_open=0\n", encoding="utf-8")
            result = runner.parse_result(path)
            self.assertEqual(result["answer"], "第一行\nerror=回答中的文字\n第二行")
            self.assertEqual(result["transcript"], "原文")
            self.assertEqual(result["error"], "")
            self.assertFalse(result["vehicle"]["action_applied"])
            self.assertFalse(result["vehicle"]["left_front_window_open"])

    def test_missing_window_receipt_is_not_assumed_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "result.txt"
            path.write_text("status=0\ntranscript=原文\nanswer=回答\nerror=\nsemantic_quality=not_automatically_verified\nvehicle_simulated=1\nclimate_on=0\naction_applied=0\n", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "required left_front_window_open"):
                runner.parse_result(path)

    def test_resource_fields_are_not_silently_defaulted(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "resources.txt"
            path.write_text("User time (seconds): 2.00\nSystem time (seconds): 0.25\nMaximum resident set size (kbytes): 42\n", encoding="utf-8")
            self.assertEqual(runner.resources(path)["max_rss_kib"], 42)
            path.write_text("", encoding="utf-8")
            with self.assertRaises(ValueError):
                runner.resources(path)

    def test_wait_exception_recovers_only_owned_process_group(self):
        for error in (KeyboardInterrupt(), OSError("wait failed")):
            process = mock.Mock(pid=123, poll=mock.Mock(return_value=None))
            process.wait.side_effect = [error, -signal.SIGKILL]
            record = {}
            with mock.patch.object(runner.os, "killpg") as kill:
                with self.assertRaises(type(error)):
                    runner.wait_owned_process(process, record, 180)
            kill.assert_called_once_with(123, signal.SIGKILL)
            self.assertTrue(record["forced_process_cleanup"])
            self.assertFalse(record["graceful_cancellation"])
            self.assertEqual(process.wait.call_count, 2)

    def test_real_timeout_reaps_process(self):
        process = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"], start_new_session=True)
        record = {}
        try:
            with self.assertRaises(subprocess.TimeoutExpired):
                runner.wait_owned_process(process, record, 0.01)
            self.assertEqual(process.returncode, -signal.SIGKILL)
            self.assertTrue(record["forced_process_cleanup"])
            self.assertFalse(record["graceful_cancellation"])
        finally:
            if process.poll() is None:
                runner.os.killpg(process.pid, signal.SIGKILL)
                process.wait()

    def test_group_cleanup_does_not_depend_on_direct_parent_liveness(self):
        for group_gone in (False, True):
            process = mock.Mock(pid=123, poll=mock.Mock(return_value=0))
            process.wait.side_effect = [KeyboardInterrupt(), 0]
            with mock.patch.object(runner.os, "killpg", side_effect=ProcessLookupError() if group_gone else None) as kill:
                with self.assertRaises(KeyboardInterrupt):
                    runner.wait_owned_process(process, {}, 180)
            kill.assert_called_once_with(123, signal.SIGKILL)
            self.assertEqual(process.wait.call_count, 2)


if __name__ == "__main__":
    unittest.main()
