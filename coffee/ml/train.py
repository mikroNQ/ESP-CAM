"""Train the drink classifier on collected samples and emit drink_model.h.

Reads PNGs from coffee/ml/data/<label>/ (produced by collect_session.py), trains
the tiny CNN from model.py, quantizes to int8 and writes
CoffeeVerifier/drink_model.h. Then recompile/flash to deploy.

Usage:
    python train.py --epochs 40
"""
import argparse
import glob
import os

import numpy as np
import tensorflow as tf
from PIL import Image

from model import (build_model, quantize_to_tflite, CLASS_NAMES,
                   INPUT_W, INPUT_H, INPUT_CH)
from convert_to_header import write_header

HERE = os.path.dirname(os.path.abspath(__file__))
DATA_DIR = os.path.join(HERE, "data")
TFLITE_PATH = os.path.join(HERE, "model.tflite")
HEADER_PATH = os.path.join(HERE, "..", "CoffeeVerifier", "drink_model.h")


def load_dataset():
    images, labels = [], []
    for idx, name in enumerate(CLASS_NAMES):
        paths = sorted(glob.glob(os.path.join(DATA_DIR, name, "*.png")))
        if not paths:
            raise SystemExit(f"No samples for class '{name}' — collect it or drop "
                             f"it from CLASS_NAMES in model.py.")
        print(f"  {name}: {len(paths)} samples")
        for p in paths:
            img = Image.open(p).convert("RGB").resize((INPUT_W, INPUT_H), Image.BOX)
            images.append(np.asarray(img, dtype=np.float32) / 255.0)  # [0,1]
            labels.append(idx)
    X = np.stack(images)
    y = np.array(labels, dtype=np.int32)
    return X, y


def augment(X, y, factor=3):
    """Synthesise variants to cover camera AGC/AWB drift, framing jitter and
    sensor noise so the model generalises beyond the exact capture conditions."""
    rng = np.random.default_rng(0)
    aug_X, aug_y = [X], [y]
    for _ in range(factor):
        n = len(X)
        b_scale = rng.uniform(0.75, 1.25, (n, 1, 1, 1)).astype(np.float32)
        b_bias = rng.uniform(-0.06, 0.06, (n, 1, 1, 1)).astype(np.float32)
        chan = rng.uniform(0.9, 1.1, (n, 1, 1, 3)).astype(np.float32)   # AWB drift
        gamma = rng.uniform(0.8, 1.25, (n, 1, 1, 1)).astype(np.float32)
        x = np.clip(X * b_scale + b_bias, 0.0, 1.0) * chan
        x = np.clip(x, 1e-6, 1.0) ** gamma
        shifted = np.empty_like(x)
        for k in range(n):
            sy = int(rng.integers(-2, 3))
            sx = int(rng.integers(-2, 3))
            shifted[k] = np.roll(x[k], (sy, sx), axis=(0, 1))
        x = shifted + rng.normal(0.0, 0.02, x.shape).astype(np.float32)
        aug_X.append(np.clip(x, 0.0, 1.0))
        aug_y.append(y)
    return np.concatenate(aug_X), np.concatenate(aug_y)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--epochs", type=int, default=40)
    ap.add_argument("--batch", type=int, default=32)
    args = ap.parse_args()

    print(f"loading dataset ({len(CLASS_NAMES)} classes: {CLASS_NAMES})")
    X, y = load_dataset()
    print(f"loaded {len(X)} samples")

    perm = np.random.default_rng(0).permutation(len(X))
    X, y = X[perm], y[perm]
    n_val = max(1, int(0.2 * len(X)))
    Xv, yv = X[:n_val], y[:n_val]
    Xt, yt = X[n_val:], y[n_val:]
    Xt, yt = augment(Xt, yt)

    model = build_model()
    model.compile(optimizer="adam",
                  loss="sparse_categorical_crossentropy",
                  metrics=["accuracy"])
    model.fit(Xt, yt, validation_data=(Xv, yv),
              epochs=args.epochs, batch_size=args.batch)

    tflite_bytes = quantize_to_tflite(model, X[:min(200, len(X))])
    with open(TFLITE_PATH, "wb") as f:
        f.write(tflite_bytes)
    write_header(TFLITE_PATH, HEADER_PATH)
    print("Trained model written to ../CoffeeVerifier/drink_model.h. Recompile + flash.")


if __name__ == "__main__":
    main()
