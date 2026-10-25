"""Train the compact hashed linear intent classifier on the host.

This is genuine optimization (weighted softmax cross-entropy + Adam), not a
keyword table. The exported device model is an int8-quantized linear classifier
over hashed word/bigram/char-ngram features.

Example:
    .venv/Scripts/python tools/train.py --epochs 500
"""

from __future__ import annotations

import argparse
import datetime as dt
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from model_pipeline import (  # noqa: E402
    CLASS_COUNT,
    INTENTS,
    INTENT_INDEX,
    classification_metrics,
    labels,
    load_rows,
    rejection_summary,
    sha256_file,
    split_by_group,
    vectorize,
    write_json,
)
from tinyai_features import (  # noqa: E402
    CHAR_NGRAM_MAX,
    CHAR_NGRAM_MIN,
    FEATURE_BUCKETS,
    MAX_TEXT_BYTES,
    MAX_TOKENS,
)

DEFAULT_CONFIDENCE = 0.50
DEFAULT_MARGIN = 0.10


def softmax(logits: np.ndarray) -> np.ndarray:
    shifted = logits - logits.max(axis=1, keepdims=True)
    exp = np.exp(shifted)
    return exp / exp.sum(axis=1, keepdims=True)


def train_softmax(
    matrix: np.ndarray,
    truth: np.ndarray,
    matrix_val: np.ndarray,
    truth_val: np.ndarray,
    *,
    epochs: int,
    learning_rate: float,
    l2: float,
    label_smoothing: float,
    seed: int,
) -> tuple[np.ndarray, np.ndarray, dict]:
    rng = np.random.default_rng(seed)
    samples, features = matrix.shape
    weights = rng.normal(0.0, 0.01, size=(CLASS_COUNT, features)).astype(np.float64)
    biases = np.zeros(CLASS_COUNT, dtype=np.float64)

    counts = np.bincount(truth, minlength=CLASS_COUNT).astype(np.float64)
    raw_weights = np.where(counts > 0, samples / (CLASS_COUNT * np.maximum(counts, 1.0)), 0.0)
    class_weights = np.clip(raw_weights, 0.5, 4.0)
    sample_weights = class_weights[truth]
    weight_sum = float(sample_weights.sum())

    m_w = np.zeros_like(weights)
    v_w = np.zeros_like(weights)
    m_b = np.zeros_like(biases)
    v_b = np.zeros_like(biases)
    beta1, beta2, eps = 0.9, 0.999, 1e-8

    best = {
        "weights": weights.copy(),
        "biases": biases.copy(),
        "epoch": 0,
        "val_accuracy": -1.0,
        "val_loss": float("inf"),
    }
    history = []

    for epoch in range(1, epochs + 1):
        logits = matrix @ weights.T + biases
        probs = softmax(logits)
        targets = np.full_like(probs, label_smoothing / CLASS_COUNT)
        targets[np.arange(samples), truth] += 1.0 - label_smoothing
        ce = -np.sum(targets * np.log(np.maximum(probs, 1e-12)), axis=1)
        loss = float(np.sum(sample_weights * ce) / weight_sum)
        loss += float(l2 * np.sum(weights * weights))

        grad_logits = probs - targets
        grad_logits *= (sample_weights / weight_sum)[:, None]
        grad_w = grad_logits.T @ matrix + 2.0 * l2 * weights
        grad_b = grad_logits.sum(axis=0)

        m_w = beta1 * m_w + (1.0 - beta1) * grad_w
        v_w = beta2 * v_w + (1.0 - beta2) * (grad_w * grad_w)
        m_b = beta1 * m_b + (1.0 - beta1) * grad_b
        v_b = beta2 * v_b + (1.0 - beta2) * (grad_b * grad_b)
        m_w_hat = m_w / (1.0 - beta1 ** epoch)
        v_w_hat = v_w / (1.0 - beta2 ** epoch)
        m_b_hat = m_b / (1.0 - beta1 ** epoch)
        v_b_hat = v_b / (1.0 - beta2 ** epoch)
        weights -= learning_rate * m_w_hat / (np.sqrt(v_w_hat) + eps)
        biases -= learning_rate * m_b_hat / (np.sqrt(v_b_hat) + eps)

        if matrix_val.shape[0] > 0:
            val_probs = softmax(matrix_val @ weights.T + biases)
            val_pred = val_probs.argmax(axis=1)
            val_accuracy = float((val_pred == truth_val).mean())
            val_ce = -np.log(np.maximum(val_probs[np.arange(len(truth_val)), truth_val], 1e-12))
            val_loss = float(val_ce.mean())
        else:
            val_accuracy = float((probs.argmax(axis=1) == truth).mean())
            val_loss = loss

        if val_accuracy > best["val_accuracy"] or (
            val_accuracy == best["val_accuracy"] and val_loss < best["val_loss"]
        ):
            best.update({
                "weights": weights.copy(),
                "biases": biases.copy(),
                "epoch": epoch,
                "val_accuracy": val_accuracy,
                "val_loss": val_loss,
            })

        if epoch == 1 or epoch % 50 == 0 or epoch == epochs:
            history.append({"epoch": epoch, "train_loss": loss, "val_accuracy": val_accuracy})
            print(f"epoch {epoch:4d}  train_loss={loss:.4f}  val_acc={val_accuracy:.4f}")

    return best["weights"], best["biases"], {
        "best_epoch": best["epoch"],
        "best_val_accuracy": best["val_accuracy"],
        "best_val_loss": best["val_loss"],
        "class_weights": class_weights.tolist(),
        "history": history,
    }


def prediction_errors(rows, truth, pred, probs, max_items=12):
    errors = []
    names = INTENTS
    for index, row in enumerate(rows):
        if int(truth[index]) == int(pred[index]):
            continue
        order = np.argsort(probs[index])[::-1]
        errors.append({
            "text": row.text,
            "expected": names[int(truth[index])],
            "predicted": names[int(pred[index])],
            "confidence": float(probs[index, order[0]]),
            "runner_up": names[int(order[1])],
            "runner_up_confidence": float(probs[index, order[1]]),
            "group": row.group,
        })
        if len(errors) >= max_items:
            break
    return errors


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dataset", type=Path, default=ROOT / "data" / "dataset.csv")
    parser.add_argument("--out", type=Path, default=ROOT / "model")
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--test-fraction", type=float, default=0.20)
    parser.add_argument("--val-fraction", type=float, default=0.10)
    parser.add_argument("--epochs", type=int, default=600)
    parser.add_argument("--learning-rate", type=float, default=0.03)
    parser.add_argument("--l2", type=float, default=2e-4)
    parser.add_argument("--label-smoothing", type=float, default=0.02)
    parser.add_argument("--confidence-threshold", type=float, default=DEFAULT_CONFIDENCE)
    parser.add_argument("--margin-threshold", type=float, default=DEFAULT_MARGIN)
    args = parser.parse_args()

    rows = load_rows(args.dataset)
    dataset_hash = sha256_file(args.dataset)
    train_rows, val_rows, test_rows, test_groups, val_groups = split_by_group(
        rows, args.seed, args.test_fraction, args.val_fraction
    )

    print(f"dataset: {len(rows)} rows, {len({r.group for r in rows})} groups")
    print(f"split: train={len(train_rows)} val={len(val_rows)} test={len(test_rows)} rows")
    print(f"test groups: {len(test_groups)}, val groups: {len(val_groups)}")

    matrix_train = vectorize(train_rows)
    matrix_val = vectorize(val_rows)
    matrix_test = vectorize(test_rows)
    y_train = labels(train_rows)
    y_val = labels(val_rows)
    y_test = labels(test_rows)

    weights, biases, training_meta = train_softmax(
        matrix_train,
        y_train,
        matrix_val,
        y_val,
        epochs=args.epochs,
        learning_rate=args.learning_rate,
        l2=args.l2,
        label_smoothing=args.label_smoothing,
        seed=args.seed,
    )

    test_probs = softmax(matrix_test @ weights.T + biases)
    test_pred = test_probs.argmax(axis=1)
    metrics = classification_metrics(y_test, test_pred)
    metrics["per_class_errors"] = prediction_errors(test_rows, y_test, test_pred, test_probs)
    metrics["rejection"] = rejection_summary(
        y_test, test_pred, test_probs, args.confidence_threshold, args.margin_threshold
    )

    val_probs = softmax(matrix_val @ weights.T + biases) if len(val_rows) else np.zeros((0, CLASS_COUNT))
    val_pred = val_probs.argmax(axis=1) if len(val_rows) else np.zeros(0, dtype=int)
    val_metrics = classification_metrics(y_val, val_pred) if len(val_rows) else {"accuracy": 0.0}

    print()
    print(f"test accuracy: {metrics['accuracy']:.4f}  macro F1: {metrics['macro_f1']:.4f}")
    for item in metrics["per_class"]:
        print(
            f"  {item['intent']:14s} P={item['precision']:.3f} "
            f"R={item['recall']:.3f} F1={item['f1']:.3f} n={item['support']}"
        )

    args.out.mkdir(parents=True, exist_ok=True)
    np.savez(
        args.out / "intent_model.npz",
        weights=weights.astype(np.float32),
        biases=biases.astype(np.float32),
        classes=np.asarray(INTENTS),
        feature_buckets=np.int32(FEATURE_BUCKETS),
        char_ngram_min=np.int32(CHAR_NGRAM_MIN),
        char_ngram_max=np.int32(CHAR_NGRAM_MAX),
        max_tokens=np.int32(MAX_TOKENS),
        max_text_bytes=np.int32(MAX_TEXT_BYTES),
        seed=np.int32(args.seed),
        dataset_sha256=np.asarray(dataset_hash),
        confidence_threshold=np.float32(args.confidence_threshold),
        margin_threshold=np.float32(args.margin_threshold),
    )

    metadata = {
        "model_type": "hashed_bow_char_ngram_linear_softmax",
        "created_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "dataset": {
            "path": str(args.dataset.relative_to(ROOT)),
            "sha256": dataset_hash,
            "rows": len(rows),
            "groups": len({r.group for r in rows}),
        },
        "features": {
            "buckets": FEATURE_BUCKETS,
            "char_ngram_min": CHAR_NGRAM_MIN,
            "char_ngram_max": CHAR_NGRAM_MAX,
            "max_tokens": MAX_TOKENS,
            "max_text_bytes": MAX_TEXT_BYTES,
        },
        "hyperparameters": {
            "seed": args.seed,
            "epochs": args.epochs,
            "learning_rate": args.learning_rate,
            "l2": args.l2,
            "label_smoothing": args.label_smoothing,
            "test_fraction": args.test_fraction,
            "val_fraction": args.val_fraction,
        },
        "split": {
            "train_rows": len(train_rows),
            "val_rows": len(val_rows),
            "test_rows": len(test_rows),
            "test_groups": sorted(test_groups),
            "val_groups": sorted(val_groups),
        },
        "training": training_meta,
        "validation": val_metrics,
        "test": metrics,
    }
    write_json(args.out / "metrics.json", metadata)
    write_json(args.out / "split.json", {
        "dataset_sha256": dataset_hash,
        "seed": args.seed,
        "test_groups": sorted(test_groups),
        "val_groups": sorted(val_groups),
        "test_indices": [index for index, row in enumerate(rows) if row.group in test_groups],
    })

    print(f"wrote {args.out / 'intent_model.npz'}")
    print(f"wrote {args.out / 'metrics.json'}")


if __name__ == "__main__":
    main()
