"""Malformed local assets must fail, not become a normal retrieval miss."""
import json
from pathlib import Path
import sys
import tempfile
import unittest

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "inference/python"))
from cabinflow_rag import load_index


class RagIndexTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name)
        self.data = {"texts": ["模拟资料"], "metadata": [{"id": 0, "section": "章节", "subsection": "小节"}],
                     "embedding_shape": [1, 768]}
        self.vectors = np.ones((1, 768), dtype=np.float32)
        self.addCleanup(self.directory.cleanup)

    def write(self):
        (self.root / "vehicle_data.json").write_text(json.dumps(self.data), encoding="utf-8")
        np.save(self.root / "vehicle_embeddings.npy", self.vectors)

    def test_valid_normalized_owned_index(self):
        self.write()
        texts, metadata, vectors = load_index(self.root)
        self.assertEqual(texts, ["模拟资料"])
        self.assertEqual(metadata[0]["id"], 0)
        self.assertAlmostEqual(float(np.linalg.norm(vectors[0])), 1, places=6)

    def test_invalid_vectors(self):
        for vectors in (np.zeros((1, 768), dtype=np.float32), np.ones((1, 767), dtype=np.float32),
                        np.ones((1, 768), dtype=np.float64), np.full((1, 768), np.nan, dtype=np.float32),
                        np.array([["unsafe object"]], dtype=object)):
            with self.subTest(dtype=str(vectors.dtype), shape=vectors.shape):
                self.vectors = vectors
                self.write()
                with self.assertRaises(ValueError):
                    load_index(self.root)

    def test_invalid_identity_and_text(self):
        for key, value in (("id", 2), ("section", ""), ("subsection", "\0")):
            with self.subTest(key=key):
                self.data["metadata"][0][key] = value
                self.write()
                with self.assertRaises(ValueError):
                    load_index(self.root)
                self.data["metadata"][0] = {"id": 0, "section": "章节", "subsection": "小节"}
        for text in ("", "bad\0text", "x" * (16 * 1024 + 1)):
            self.data["texts"] = [text]
            self.write()
            with self.assertRaises(ValueError):
                load_index(self.root)

    def test_bad_file_is_not_empty_index(self):
        self.write()
        (self.root / "vehicle_data.json").write_text("{broken", encoding="utf-8")
        with self.assertRaises(ValueError):
            load_index(self.root)
        self.write()
        (self.root / "vehicle_embeddings.npy").write_bytes(b"broken")
        with self.assertRaises(ValueError):
            load_index(self.root)


if __name__ == "__main__":
    unittest.main()
