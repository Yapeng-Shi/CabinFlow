import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import generate_demo_speech as fixtures
sys.path.pop(0)


class DemoSpeechTest(unittest.TestCase):
    def test_punctuation_and_whitespace_are_not_word_errors(self):
        self.assertEqual(fixtures.normalize_transcript("请打开空调。 \n"), "请打开空调")

    def test_numbers_are_not_silently_rewritten(self):
        self.assertNotEqual(fixtures.normalize_transcript("一加一"), "1加1")

    def test_registered_five_cases_are_valid(self):
        inputs = json.loads(fixtures.INPUTS.read_text(encoding="utf-8"))
        fixtures.validate_cases(inputs)
        self.assertEqual(len(inputs["cases"]), 5)

    def test_duplicate_or_unsafe_id_is_rejected(self):
        case = {"id": "ok", "text": "你好。", "reference_text": "你好"}
        with self.assertRaises(ValueError):
            fixtures.validate_cases({"cases": [case, case]})
        with self.assertRaises(ValueError):
            fixtures.validate_cases({"cases": [dict(case, id="../old")]})

    def test_wrong_reference_is_not_accepted(self):
        with self.assertRaises(ValueError):
            fixtures.validate_cases({"cases": [{"id": "ok", "text": "打开空调", "reference_text": "关闭空调"}]})


if __name__ == "__main__":
    unittest.main()
