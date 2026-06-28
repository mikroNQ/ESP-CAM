"""Shared model definition + int8 quantization for the drink classifier (STAGE 2).

STAGE 1 (the shipped MVP) needs no model at all: the firmware decides
"dispensed / not dispensed" from a baseline delta and milk/no-milk from a
luminance threshold (cup_verifier.cpp::classify_milk). This file is the seam for
STAGE 2 — a tiny on-device CNN that classifies the *drink* more robustly than the
luminance heuristic, e.g. when foam/crema confuses the threshold.

Architecture and preprocessing are deliberately identical to the parent LED
project (../../ml/model.py): 24x24x3 RGB input, box-downscaled ROI, normalised to
[0,1], full-int8 quantization. Only the classes differ. Reuse the parent's proven
collect_session.py / train.py / convert_to_header.py with this model.py in place
(see README.md) to emit a drink_model.h, then wire it into classify_milk().

The financial decision (dispensed/not) stays on the baseline delta — a CNN
misclassification here degrades drink-type accuracy, never the money verdict.
"""
import numpy as np
import tensorflow as tf

# Class order MUST match the consumer in cup_verifier.cpp and the data/<label>/
# folder names. EDIT THIS to your actual machine menu — every change here means
# recollecting. "empty" cross-checks the baseline-delta money axis (нет кофе);
# the rest are the drink types the CNN reports (есть кофе/капучино/латте/чай).
# NOTE: hard pairs to give the most/cleanest data — cappuccino vs latte (both
# milk+coffee, differ by foam, subtle top-down) and cacao vs coffee (both dark
# brown). A milky cacao can also look like cappuccino/latte; keep cups/recipe
# consistent so the colour separates them.
CLASS_NAMES = ["empty", "coffee", "cappuccino", "latte", "tea", "cacao"]
NUM_CLASSES = len(CLASS_NAMES)

# Model input geometry. Keep tiny so it runs software-only on the ESP32.
INPUT_H = 24
INPUT_W = 24
INPUT_CH = 3


def build_model():
    """A tiny RGB CNN. Telling drinks apart is a colour/texture job, hence 3 channels.
    Bump the conv widths (8->16, 16->32) if the classes underfit — check arena size."""
    inputs = tf.keras.Input(shape=(INPUT_H, INPUT_W, INPUT_CH))
    x = tf.keras.layers.Conv2D(8, 3, padding="same", activation="relu")(inputs)
    x = tf.keras.layers.MaxPooling2D()(x)
    x = tf.keras.layers.SeparableConv2D(16, 3, padding="same", activation="relu")(x)
    x = tf.keras.layers.MaxPooling2D()(x)
    x = tf.keras.layers.GlobalAveragePooling2D()(x)
    outputs = tf.keras.layers.Dense(NUM_CLASSES, activation="softmax")(x)
    return tf.keras.Model(inputs, outputs)


def quantize_to_tflite(model, representative_images):
    """Full-integer (int8 in / int8 out) post-training quantization.

    representative_images: float32 array [N, H, W, 3] normalised to [0,1],
    matching the firmware preprocessing (pixel/255).
    """
    def rep_dataset():
        for i in range(len(representative_images)):
            yield [representative_images[i:i + 1].astype(np.float32)]

    converter = tf.lite.TFLiteConverter.from_keras_model(model)
    converter.optimizations = [tf.lite.Optimize.DEFAULT]
    converter.representative_dataset = rep_dataset
    converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    converter.inference_input_type = tf.int8
    converter.inference_output_type = tf.int8
    return converter.convert()
