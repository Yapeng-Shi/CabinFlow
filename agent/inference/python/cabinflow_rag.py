"""Original Transformers mean-pooling retrieval, using only explicit local assets."""

import json
from pathlib import Path

import numpy as np
import torch
import transformers
from transformers import AutoTokenizer, BertModel


def validate_environment(site_packages):
    root = Path(site_packages).resolve(strict=True)
    for module, expected in ((np, "2.2.6"), (torch, "2.7.1+cpu"),
                             (transformers, "4.51.3")):
        if module.__version__ != expected or not Path(module.__file__).resolve().is_relative_to(root):
            raise ValueError(f"unexpected RAG dependency: {module.__name__}")


def load_index(directory):
    root = Path(directory).resolve(strict=True)
    metadata_path = root / "vehicle_data.json"
    vectors_path = root / "vehicle_embeddings.npy"
    if any(path.stat().st_size > 16 * 1024 * 1024 for path in (metadata_path, vectors_path)):
        raise ValueError("RAG index file exceeds limit")
    data = json.loads(metadata_path.read_text(encoding="utf-8"))
    vectors = np.load(vectors_path, allow_pickle=False)
    texts, metadata = data["texts"], data["metadata"]
    if (vectors.dtype != np.dtype("float32") or vectors.ndim != 2 or
            vectors.shape[1] != 768 or not 0 < vectors.shape[0] <= 10000 or
            vectors.shape != tuple(data["embedding_shape"]) or
            len(texts) != len(metadata) or len(texts) != vectors.shape[0] or
            not np.isfinite(vectors).all()):
        raise ValueError("invalid RAG index shape or vectors")
    for row, (text, item) in enumerate(zip(texts, metadata)):
        if (not isinstance(text, str) or not text or "\0" in text or
                len(text.encode("utf-8")) > 16 * 1024 or item["id"] != row or
                any(not isinstance(item[key], str) or not item[key] or "\0" in item[key]
                    for key in ("section", "subsection"))):
            raise ValueError("invalid RAG document metadata")
    norms = np.linalg.norm(vectors, axis=1, keepdims=True)
    if not np.isfinite(norms).all() or (norms <= 0).any():
        raise ValueError("invalid RAG vector norm")
    return texts, metadata, vectors / norms


class Searcher:
    def __init__(self, site_packages, model_directory, index_directory, threads):
        validate_environment(site_packages)
        if threads <= 0:
            raise ValueError("RAG threads must be positive")
        torch.set_num_threads(threads)
        self.texts, self.metadata, self.vectors = load_index(index_directory)
        # 不读取旧 pickle，也不在运行时下载、重建索引或尝试其他模型。
        self.tokenizer = AutoTokenizer.from_pretrained(model_directory, local_files_only=True)
        self.model = BertModel.from_pretrained(model_directory, local_files_only=True).cpu().eval()
        if self.model.config.hidden_size != 768 or self.tokenizer.vocab_size != 21128:
            raise ValueError("unexpected RAG embedding model")

    def embed(self, text):
        encoded = self.tokenizer(text, return_tensors="pt", padding=True,
                                 truncation=True, max_length=128)
        with torch.inference_mode():
            hidden = self.model(**encoded).last_hidden_state
            mask = encoded["attention_mask"].unsqueeze(-1).expand(hidden.size()).float()
            vector = (hidden * mask).sum(1) / mask.sum(1).clamp(min=1e-9)
        vector = vector[0].numpy()
        norm = np.linalg.norm(vector)
        if not np.isfinite(vector).all() or not np.isfinite(norm) or norm <= 0:
            raise ValueError("invalid query embedding")
        return vector / norm

    def search(self, query):
        if not query or "\0" in query or len(query.encode("utf-8")) > 16 * 1024:
            raise ValueError("invalid RAG query")
        scores = self.vectors @ self.embed(query)
        if not np.isfinite(scores).all():
            raise ValueError("invalid similarity")
        row = int(np.argsort(scores)[::-1][0])
        score = float(scores[row])
        # 正常无命中是显式结果；异常不转换成 None，避免误触发 HYBRID → LLM。
        if score < 0.5:
            return None
        item = self.metadata[row]
        return {"id": row, "section": item["section"], "subsection": item["subsection"],
                "text": self.texts[row], "similarity": score}
