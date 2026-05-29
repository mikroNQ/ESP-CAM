"""Generate a bootstrap (untrained) led_model.h so the firmware compiles and
runs end-to-end before any real scanner footage has been collected.

The model has the correct shape and is properly int8-quantized, but random
weights — it will classify nonsense until you train on real data with train.py.
Its only job is to let you wire up and test timing + TCP output.

Usage:
    python make_bootstrap.py
"""
import os
import numpy as np

from model import build_model, quantize_to_tflite, INPUT_H, INPUT_W, INPUT_CH
from convert_to_header import write_header

HERE = os.path.dirname(os.path.abspath(__file__))
TFLITE_PATH = os.path.join(HERE, "model.tflite")
HEADER_PATH = os.path.join(HERE, "..", "CameraWebServer", "led_model.h")


def main():
    np.random.seed(0)
    model = build_model()
    # Random representative data so the quantizer can pick scales/zero-points.
    rep = np.random.rand(64, INPUT_H, INPUT_W, INPUT_CH).astype(np.float32)
    tflite_bytes = quantize_to_tflite(model, rep)
    with open(TFLITE_PATH, "wb") as f:
        f.write(tflite_bytes)
    write_header(TFLITE_PATH, HEADER_PATH)
    print("Bootstrap model written. Remember: it is UNTRAINED.")


if __name__ == "__main__":
    main()
