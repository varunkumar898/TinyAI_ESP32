"""Evaluate the trained model, including int8 quantization and the challenge set."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from model_pipeline import (  # noqa: E402
    INTENTS,
    INTENT_INDEX,
    classification_metrics,
    labels,
    load_challenge,
    load_rows,
    predict_quantized,
    quantize,
    rejection_summary,
    sha256_file,
    split_from_groups,
    vectorize,
    write_json,
)
from tinyai_features import (  # noqa: E402
    FEATURE_BUCKETS,
    TextError,
    mask_targets,
    tokenize,
)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", type=Path, default=ROOT / "model" / "intent_model.npz")
    parser.add_argument("--dataset", type=Path, default=ROOT / "data" / "dataset.csv")
    parser.add_argument("--challenge", type=Path, default=ROOT / "data" / "challenge.csv")
    parser.add_argument("--split", type=Path, default=ROOT / "model" / "split.json")
    parser.add_argument("--out", type=Path, default=ROOT / "model" / "evaluation.json")
    args = parser.parse_args()

    bundle = np.load(args.model, allow_pickle=False)
    weights = bundle["weights"].astype(np.float64)
    biases = bundle["biases"].astype(np.float64)
    dataset_hash = str(bundle["dataset_sha256"])
    if sha256_file(args.dataset) != dataset_hash:
        raise SystemExit("dataset changed since training; retrain before evaluating")

    split = json.loads(args.split.read_text(encoding="utf-8"))
    rows = load_rows(args.dataset)
    _, _, test_rows = split_from_groups(rows, set(split["test_groups"]), set(split["val_groups"]))
    matrix_test = vectorize(test_rows)
    y_test = labels(test_rows)

    weights_q, biases_q, scale = quantize(weights, biases)
    logits_int, probs_q = predict_quantized(matrix_test, weights_q, biases_q, scale)
    pred_q = probs_q.argmax(axis=1)
    quant_metrics = classification_metrics(y_test, pred_q)
    confidence_threshold = float(bundle["confidence_threshold"])
    margin_threshold = float(bundle["margin_threshold"])
    quant_metrics["rejection"] = rejection_summary(
        y_test, pred_q, probs_q, confidence_threshold, margin_threshold
    )
    quant_metrics["per_class_errors"] = []

    float_probs = np.exp(
        (matrix_test @ weights.T + biases)
        - (matrix_test @ weights.T + biases).max(axis=1, keepdims=True)
    )
    float_probs /= float_probs.sum(axis=1, keepdims=True)
    pred_float = float_probs.argmax(axis=1)
    float_metrics = classification_metrics(y_test, pred_float)
    float_metrics["rejection"] = rejection_summary(
        y_test, pred_float, float_probs, confidence_threshold, margin_threshold
    )

    for index, row in enumerate(test_rows):
        if int(pred_q[index]) == int(y_test[index]):
            continue
        order = np.argsort(probs_q[index])[::-1]
        quant_metrics["per_class_errors"].append({
            "text": row.text,
            "expected": INTENTS[int(y_test[index])],
            "predicted": INTENTS[int(pred_q[index])],
            "confidence": float(probs_q[index, order[0]]),
            "runner_up": INTENTS[int(order[1])],
            "group": row.group,
        })
        if len(quant_metrics["per_class_errors"]) >= 30:
            break

    challenge_rows = load_challenge(args.challenge)
    challenge_matrix = vectorize(challenge_rows)
    _, challenge_probs = predict_quantized(challenge_matrix, weights_q, biases_q, scale)
    challenge_pred = challenge_probs.argmax(axis=1)
    challenge_results = []
    for index, row in enumerate(challenge_rows):
        pred = int(challenge_pred[index])
        challenge_results.append({
            "text": row.text,
            "expected": row.intent,
            "predicted": INTENTS[pred],
            "correct": INTENTS[pred] == row.intent,
            "confidence": float(challenge_probs[index, pred]),
        })
    challenge_correct = sum(1 for item in challenge_results if item["correct"])

    payload = {
        "model": {
            "path": str(args.model.relative_to(ROOT)),
            "dataset_sha256": dataset_hash,
            "feature_buckets": int(bundle["feature_buckets"]),
            "quantization_scale": scale,
            "weight_int8_bytes": int(weights_q.size),
            "bias_int32_bytes": int(biases_q.nbytes),
            "confidence_threshold": confidence_threshold,
            "margin_threshold": margin_threshold,
        },
        "test": {"float": float_metrics, "quantized": quant_metrics},
        "challenge": {
            "path": str(args.challenge.relative_to(ROOT)),
            "count": len(challenge_rows),
            "correct": challenge_correct,
            "accuracy": challenge_correct / len(challenge_rows) if challenge_rows else 0.0,
            "results": challenge_results,
        },
    }
    write_json(args.out, payload)

    print(f"float     test accuracy: {float_metrics['accuracy']:.4f}  macro F1: {float_metrics['macro_f1']:.4f}")
    print(f"quantized test accuracy: {quant_metrics['accuracy']:.4f}  macro F1: {quant_metrics['macro_f1']:.4f}")
    print(f"quantized accepted: {quant_metrics['rejection']['accepted']}/{len(test_rows)} "
          f"({quant_metrics['rejection']['coverage']:.1%}), "
          f"accepted correct: {quant_metrics['rejection']['accepted_correct']}, "
          f"accepted incorrect: {quant_metrics['rejection']['accepted_incorrect']}")
    print()
    print("per-class (quantized):")
    for item in quant_metrics["per_class"]:
        print(
            f"  {item['intent']:14s} P={item['precision']:.3f} "
            f"R={item['recall']:.3f} F1={item['f1']:.3f} n={item['support']}"
        )
    print()
    print(f"challenge set: {challenge_correct}/{len(challenge_rows)} intent-correct")
    for item in challenge_results:
        marker = "OK " if item["correct"] else "ERR"
        print(
            f"  {marker} {item['expected']:14s} -> {item['predicted']:14s} "
            f"({item['confidence']:.3f})  {item['text']}"
        )
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
