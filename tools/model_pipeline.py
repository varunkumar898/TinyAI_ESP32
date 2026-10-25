"""Shared host-side data, split, quantization and metric helpers."""

from __future__ import annotations

import csv
import hashlib
import json
import random
from dataclasses import dataclass
from pathlib import Path

import numpy as np

from tinyai_features import (
    CHAR_NGRAM_MAX,
    CHAR_NGRAM_MIN,
    FEATURE_BUCKETS,
    INTENTS,
    INTENT_INDEX,
    MAX_TEXT_BYTES,
    MAX_TOKENS,
    TextError,
    feature_counts,
    mask_targets,
    tokenize,
)

CLASS_COUNT = len(INTENTS)


@dataclass(frozen=True)
class Row:
    text: str
    intent: str
    group: str


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(65536), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_rows(path: Path) -> list[Row]:
    rows: list[Row] = []
    with path.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        if reader.fieldnames is None or not {"text", "intent", "group"}.issubset(reader.fieldnames):
            raise SystemExit(f"{path}: expected columns text,intent,group")
        for line, item in enumerate(reader, start=2):
            text = (item["text"] or "").strip()
            intent = (item["intent"] or "").strip()
            group = (item["group"] or "").strip()
            if not text or intent not in INTENT_INDEX or not group:
                raise SystemExit(f"{path}:{line}: invalid row {item!r}")
            try:
                tokens = tokenize(text)
            except TextError as exc:
                raise SystemExit(f"{path}:{line}: tokenizer rejected text ({exc.status})") from exc
            if len(tokens) > MAX_TOKENS:
                raise SystemExit(f"{path}:{line}: too many tokens")
            rows.append(Row(text, intent, group))
    if not rows:
        raise SystemExit(f"{path}: no rows")
    return rows


def load_challenge(path: Path) -> list[Row]:
    rows: list[Row] = []
    with path.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        if reader.fieldnames is None or not {"text", "intent"}.issubset(reader.fieldnames):
            raise SystemExit(f"{path}: expected columns text,intent")
        for line, item in enumerate(reader, start=2):
            text = (item["text"] or "").strip()
            intent = (item["intent"] or "").strip()
            if not text or intent not in INTENT_INDEX:
                raise SystemExit(f"{path}:{line}: invalid row {item!r}")
            try:
                tokenize(text)
            except TextError as exc:
                raise SystemExit(f"{path}:{line}: tokenizer rejected text ({exc.status})") from exc
            rows.append(Row(text, intent, "challenge"))
    return rows


def split_by_group(rows: list[Row], seed: int, test_fraction: float, val_fraction: float):
    """Stratified split where every group lands in exactly one split."""
    if not (0.0 <= test_fraction < 1.0 and 0.0 <= val_fraction < 1.0):
        raise SystemExit("split fractions must be in [0, 1)")
    if test_fraction + val_fraction >= 0.9:
        raise SystemExit("test+val fraction must leave a training set")

    rng = random.Random(seed)
    groups_by_intent: dict[str, list[str]] = {}
    for row in rows:
        groups_by_intent.setdefault(row.intent, [])
        if row.group not in groups_by_intent[row.intent]:
            groups_by_intent[row.intent].append(row.group)

    test_groups: set[str] = set()
    val_groups: set[str] = set()
    for intent in INTENTS:
        groups = sorted(groups_by_intent.get(intent, []))
        rng.shuffle(groups)
        count = len(groups)
        if count == 0:
            continue
        n_test = max(1, round(count * test_fraction)) if test_fraction > 0 else 0
        n_val = max(1, round(count * val_fraction)) if val_fraction > 0 else 0
        # Always leave at least one training group.
        while n_test + n_val >= count and n_test + n_val > 0:
            if n_test >= n_val and n_test > 0:
                n_test -= 1
            elif n_val > 0:
                n_val -= 1
        test_groups.update(groups[:n_test])
        val_groups.update(groups[n_test:n_test + n_val])

    def subset(selected_groups: set[str]):
        return [row for row in rows if row.group in selected_groups]

    train = [row for row in rows if row.group not in test_groups and row.group not in val_groups]
    test = subset(test_groups)
    val = subset(val_groups)
    if not train or not test:
        raise SystemExit("split produced an empty train or test set")
    return train, val, test, test_groups, val_groups


def split_from_groups(rows: list[Row], test_groups: set[str], val_groups: set[str]):
    train = [r for r in rows if r.group not in test_groups and r.group not in val_groups]
    val = [r for r in rows if r.group in val_groups]
    test = [r for r in rows if r.group in test_groups]
    return train, val, test


def vectorize(rows: list[Row], buckets: int = FEATURE_BUCKETS) -> np.ndarray:
    matrix = np.zeros((len(rows), buckets), dtype=np.float32)
    for index, row in enumerate(rows):
        counts = feature_counts(mask_targets(tokenize(row.text)))
        for bucket, count in counts.items():
            matrix[index, bucket] = count
    return matrix


def labels(rows: list[Row]) -> np.ndarray:
    return np.asarray([INTENT_INDEX[row.intent] for row in rows], dtype=np.int64)


def quantize(weights: np.ndarray, biases: np.ndarray):
    max_abs = float(np.max(np.abs(weights))) if weights.size else 0.0
    if max_abs <= 0.0:
        raise SystemExit("model weights are all zero")
    scale = max_abs / 127.0
    weights_q = np.clip(np.rint(weights / scale), -127, 127).astype(np.int8)
    biases_q = np.rint(biases / scale).astype(np.int32)
    return weights_q, biases_q, float(scale)


def quantized_logits(matrix: np.ndarray, weights_q: np.ndarray, biases_q: np.ndarray) -> np.ndarray:
    # Counts are small integers; int64 avoids overflow on long host inputs.
    return matrix.astype(np.int64) @ weights_q.astype(np.int64).T + biases_q[None, :]


def softmax_from_int(logits_int: np.ndarray, scale: float) -> np.ndarray:
    scaled = logits_int.astype(np.float64) * scale
    scaled -= scaled.max(axis=1, keepdims=True)
    exp = np.exp(scaled)
    return exp / exp.sum(axis=1, keepdims=True)


def predict(matrix: np.ndarray, weights: np.ndarray, biases: np.ndarray):
    logits = matrix @ weights.T + biases
    probs = softmax_from_int(logits, 1.0)  # scale=1 because logits already float
    # The helper's "int" naming is historical; it only multiplies by scale.
    top = probs.argmax(axis=1)
    return top, probs


def predict_quantized(matrix: np.ndarray, weights_q: np.ndarray, biases_q: np.ndarray, scale: float):
    logits_int = quantized_logits(matrix, weights_q, biases_q)
    probs = softmax_from_int(logits_int, scale)
    return logits_int, probs


def classification_metrics(y_true: np.ndarray, y_pred: np.ndarray) -> dict:
    classes = CLASS_COUNT
    confusion = np.zeros((classes, classes), dtype=np.int64)
    for truth, pred in zip(y_true, y_pred):
        confusion[int(truth), int(pred)] += 1

    per_class = []
    for index, name in enumerate(INTENTS):
        tp = int(confusion[index, index])
        fp = int(confusion[:, index].sum() - tp)
        fn = int(confusion[index, :].sum() - tp)
        support = int(confusion[index, :].sum())
        precision = tp / (tp + fp) if tp + fp else 0.0
        recall = tp / (tp + fn) if tp + fn else 0.0
        f1 = (2 * precision * recall / (precision + recall)) if precision + recall else 0.0
        per_class.append({
            "intent": name,
            "precision": precision,
            "recall": recall,
            "f1": f1,
            "support": support,
        })

    supported = [item for item in per_class if item["support"] > 0]
    macro_f1 = sum(item["f1"] for item in supported) / len(supported) if supported else 0.0
    macro_precision = sum(item["precision"] for item in supported) / len(supported) if supported else 0.0
    macro_recall = sum(item["recall"] for item in supported) / len(supported) if supported else 0.0
    accuracy = float(np.trace(confusion) / confusion.sum()) if confusion.sum() else 0.0
    return {
        "accuracy": accuracy,
        "macro_precision": macro_precision,
        "macro_recall": macro_recall,
        "macro_f1": macro_f1,
        "confusion_matrix": confusion.tolist(),
        "per_class": per_class,
    }


def rejection_summary(y_true: np.ndarray, y_pred: np.ndarray, probs: np.ndarray,
                      confidence_threshold: float, margin_threshold: float) -> dict:
    sorted_probs = np.sort(probs, axis=1)
    confidence = sorted_probs[:, -1]
    margin = sorted_probs[:, -1] - sorted_probs[:, -2]
    unknown = y_pred == INTENT_INDEX["UNKNOWN"]
    rejected = unknown | (confidence < confidence_threshold) | (margin < margin_threshold)
    accepted = ~rejected
    accepted_correct = int(np.sum(accepted & (y_pred == y_true)))
    return {
        "confidence_threshold": confidence_threshold,
        "margin_threshold": margin_threshold,
        "accepted": int(accepted.sum()),
        "accepted_correct": accepted_correct,
        "accepted_incorrect": int(accepted.sum()) - accepted_correct,
        "rejected": int(rejected.sum()),
        "coverage": float(accepted.mean()) if len(y_true) else 0.0,
    }


def escaped_c_string(text: str) -> str:
    return json.dumps(text, ensure_ascii=True)


def write_json(path: Path, payload: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as handle:
        json.dump(payload, handle, indent=2, sort_keys=True)
        handle.write("\n")
