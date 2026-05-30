"""Colour/brightness heuristic that auto-labels an LED-line ROI crop.

Used by collect.py to pre-sort frames into off/red_on/white_on before you review
them. It is intentionally simple — the CNN trained on the reviewed result is
what generalises; this just removes most of the manual sorting.

Returns one of CLASS_NAMES, or None when the crop is ambiguous (so collect.py
can drop it into the _unsure pile for you to look at first).
"""
import numpy as np

# Defaults assume a FIXED exposure (set aec=0/agc=0/awb=0 via /control). Tune
# from collect.py CLI if your scene is brighter/darker.
DEFAULT_OFF_V = 45.0       # mean brightness below this => LEDs off
DEFAULT_RED_MARGIN = 20.0  # R minus max(G,B) above this => red
DEFAULT_WHITE_MIN = 110.0  # all channels above this => white


def roi_stats(arr):
    """Mean R, G, B over the crop. arr: HxWx3 uint8 (or float)."""
    a = np.asarray(arr, dtype=np.float32)
    r = float(a[..., 0].mean())
    g = float(a[..., 1].mean())
    b = float(a[..., 2].mean())
    return r, g, b


def classify_roi(arr, off_v=DEFAULT_OFF_V, red_margin=DEFAULT_RED_MARGIN,
                 white_min=DEFAULT_WHITE_MIN):
    """Return (label_or_None, stats_dict)."""
    r, g, b = roi_stats(arr)
    v = (r + g + b) / 3.0
    redness = r - max(g, b)
    stats = {"r": r, "g": g, "b": b, "v": v, "redness": redness}

    if v < off_v:
        return "off", stats
    if redness > red_margin:
        return "red_on", stats
    if min(r, g, b) > white_min:
        return "white_on", stats
    return None, stats  # ambiguous -> review
