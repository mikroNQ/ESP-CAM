"""Shared model definition + int8 quantization for the LED-line classifier.

The same architecture is used by make_bootstrap.py (random weights, so the
firmware compiles before real data exists) and train.py (trained on collected
samples). Keeping it here guarantees the input shape and class order never
drift between the two.
"""
import numpy as np
import tensorflow as tf

# Class order MUST match led_detector.h (led_state_t).
CLASS_NAMES = ["off", "red_on", "white_on"]
NUM_CLASSES = len(CLASS_NAMES)

# Model input geometry. Keep tiny: this runs software-only on an ESP32 (no
# ESP-NN acceleration via Chirale_TensorFlowLite) with no PSRAM.
INPUT_H = 24
INPUT_W = 24
INPUT_CH = 3


def build_model():
    """A tiny RGB CNN. RED vs WHITE needs colour, hence 3-channel input."""
    inputs = tf.keras.Input(shape=(INPUT_H, INPUT_W, INPUT_CH))
    x = tf.keras.layers.Conv2D(8, 3, padding="same", activation="relu")(inputs)
    x = tf.keras.layers.MaxPooling2D()(x)
    # Depthwise-separable block to keep parameter/arena cost low.
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
