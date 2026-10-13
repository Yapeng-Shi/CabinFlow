#!/usr/bin/env python3
"""离线迁移对照：原索引 + Transformers 参考 + 生产 Python/C++ 适配器。"""
import argparse
import json
from pathlib import Path
import subprocess
import sys

import numpy as np
import torch
from transformers import AutoTokenizer, BertModel


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--native-probe", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=False)
    repo = args.repo.resolve(strict=True)
    site = repo / "runtime/build/inference/rag-python/venv/lib/python3.12/site-packages"
    model_dir = repo / "runtime/build/models/text2vec-base-chinese/hf"
    index_dir = repo / "agent/automotive_edge_rag/python/vector_db"
    sys.path.insert(0, str(repo / "agent/inference/python"))
    from cabinflow_rag import Searcher
    active = Searcher(str(site), str(model_dir), str(index_dir), 2)
    documents = json.loads((index_dir / "vehicle_data.json").read_text(encoding="utf-8"))["texts"]
    index = np.load(index_dir / "vehicle_embeddings.npy", allow_pickle=False)
    assert index.shape == (45, 768) and np.isfinite(index).all()
    index = index / np.linalg.norm(index, axis=1, keepdims=True)
    tokenizer = AutoTokenizer.from_pretrained(model_dir, local_files_only=True)
    model = BertModel.from_pretrained(model_dir, local_files_only=True).eval()
    cases = [(f"document-{i}", text) for i, text in enumerate(documents)] + [
        ("engine-warning", "发动机警告灯亮了怎么办？"),
        ("climate-maintenance", "空调滤清器多久更换？"),
        ("cruise-control", "定速巡航怎么使用？"),
        ("no-hit", "量子纠缠的贝尔不等式是什么？"),
        ("mixed", "ABS 警告灯亮了，Bluetooth 怎么连接？"),
        ("symbols", "机油 5W-30 / 10000km? A/C < > [ ] + ="),
        ("literal-special", "[CLS]机油[SEP][UNK][MASK]"),
        ("long-word", "a" * 101),
        ("composed-accent", "é 空调"),
        ("decomposed-accent", "e\u0301 空调"),
        ("content126", "车" * 126),
        ("content127", "车" * 127),
    ]
    # 不修改此前失败候选的固定容差；每条都实际测量，未观察不记为通过。
    tolerance = 1e-4
    records = []
    for number, (name, text) in enumerate(cases):
        encoded = tokenizer(text, truncation=True, max_length=128, return_tensors="pt")
        with torch.no_grad():
            hidden = model(**encoded).last_hidden_state
            mask = encoded["attention_mask"].unsqueeze(-1)
            reference = ((hidden * mask).sum(1) / mask.sum(1))[0].numpy()
        reference /= np.linalg.norm(reference)
        similarities = index @ reference
        best = int(np.argsort(similarities)[::-1][0])
        expected_id = best if float(similarities[best]) >= 0.5 else None
        record = {"name": name, "query": text, "expected_id": expected_id,
                  "token_ids_match": active.tokenizer(text, truncation=True, max_length=128)["input_ids"] == encoded["input_ids"][0].tolist(),
                  "embedding_max_abs_error": float(np.max(np.abs(active.embed(text) - reference)))}
        if number < 45:
            record["original_index_max_abs_error"] = float(np.max(np.abs(index[number] - reference)))
        child = subprocess.run([str(args.native_probe.resolve(strict=True)), str(site), str(model_dir), str(index_dir), text],
                               capture_output=True, text=True, timeout=60)
        prefix = args.output_dir / f"{number:02d}-{name}"
        prefix.with_suffix(".stdout").write_text(child.stdout, encoding="utf-8")
        prefix.with_suffix(".stderr").write_text(child.stderr, encoding="utf-8")
        record["native_exit"] = child.returncode
        record["passed"] = False
        if child.returncode == 0:
            found = json.loads(child.stdout)
            actual_id = found["document"]["id"] if found["matched"] else None
            record["native_id"] = actual_id
            record["native_rank_matches"] = actual_id == expected_id
            record["native_score_error"] = abs(found["document"]["similarity"] - float(similarities[best])) if found["matched"] else 0
            record["passed"] = (record["native_rank_matches"] and record["token_ids_match"] and
                                record["native_score_error"] <= tolerance and
                                record["embedding_max_abs_error"] <= tolerance and
                                record.get("original_index_max_abs_error", 0) <= tolerance)
        records.append(record)
        print(json.dumps(record, ensure_ascii=False), flush=True)
    for name, site_arg, model_arg, index_arg in [
        ("missing-site", args.output_dir / "missing", model_dir, index_dir),
        ("missing-model", site, args.output_dir, index_dir),
        ("missing-index", site, model_dir, args.output_dir),
    ]:
        child = subprocess.run([str(args.native_probe.resolve()), str(site_arg), str(model_arg), str(index_arg), "查询"],
                               capture_output=True, text=True, timeout=60)
        (args.output_dir / (name + ".stderr")).write_text(child.stderr, encoding="utf-8")
        records.append({"name": name, "passed": child.returncode == 1 and "rag_probe failed:" in child.stderr,
                        "native_exit": child.returncode})
    summary = {"tolerance": tolerance, "torch": torch.__version__, "numpy": np.__version__, "cpu_threads": 2,
               "retrieval_cases": 57, "startup_error_cases": 3, "cases": records, "passed": all(x["passed"] for x in records)}
    (args.output_dir / "results.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
    return 0 if summary["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
