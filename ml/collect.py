"""Collect ROI samples from a running ESP32-CAM for training.

Pulls frames from the camera's /bmp endpoint and crops the configured ROI. Two
modes:

  --auto   : grab continuously and auto-sort each crop into off/red_on/white_on
             by the colour heuristic (autolabel.py); ambiguous crops go to _unsure.
             Then run review.py to fix mistakes. RECOMMENDED.

  --label X: save every crop into data/X/ (use when you can hold one LED state).

Crops are saved at native ROI resolution (not yet downscaled); train.py resizes
them to the model input with box filtering to match the firmware preprocessing.

Examples:
    python collect.py --host 192.168.1.50 --auto --count 600
    python collect.py --host 192.168.1.50 --label red_on --count 200

The ROI defaults match config.h; override with --roi x y w h (query the camera's
/detcfg to see the live ROI).
"""
import argparse
import io
import os
import time

import requests
from PIL import Image

from model import CLASS_NAMES
from autolabel import classify_roi, DEFAULT_OFF_V, DEFAULT_RED_MARGIN, DEFAULT_WHITE_MIN

HERE = os.path.dirname(os.path.abspath(__file__))

# Defaults mirror CameraWebServer/config.h.
DEFAULT_ROI = (16, 40, 128, 40)
UNSURE_DIR = "_unsure"


def fetch_bmp(host):
    r = requests.get(f"http://{host}/bmp", timeout=5)
    r.raise_for_status()
    return Image.open(io.BytesIO(r.content)).convert("RGB")


def next_index(out_dir):
    os.makedirs(out_dir, exist_ok=True)
    return len([f for f in os.listdir(out_dir) if f.endswith(".png")])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", required=True, help="camera IP or host")
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--auto", action="store_true", help="auto-label by colour heuristic")
    g.add_argument("--label", choices=CLASS_NAMES, help="fixed label for every crop")
    ap.add_argument("--count", type=int, default=400)
    ap.add_argument("--interval", type=float, default=0.1, help="seconds between grabs")
    ap.add_argument("--roi", type=int, nargs=4, metavar=("X", "Y", "W", "H"),
                    default=DEFAULT_ROI)
    ap.add_argument("--off-v", type=float, default=DEFAULT_OFF_V)
    ap.add_argument("--red-margin", type=float, default=DEFAULT_RED_MARGIN)
    ap.add_argument("--white-min", type=float, default=DEFAULT_WHITE_MIN)
    args = ap.parse_args()

    x, y, w, h = args.roi
    data_dir = os.path.join(HERE, "data")
    counters = {}  # per-folder running index

    def save(label, crop):
        out_dir = os.path.join(data_dir, label)
        if label not in counters:
            counters[label] = next_index(out_dir)
        crop.save(os.path.join(out_dir, f"{counters[label]:05d}.png"))
        counters[label] += 1

    mode = "auto-label" if args.auto else f"label='{args.label}'"
    print(f"Collecting {args.count} crops from {args.host} ROI=({x},{y},{w},{h}) [{mode}]")
    tally = {}
    saved = 0
    while saved < args.count:
        try:
            img = fetch_bmp(args.host)
            crop = img.crop((x, y, x + w, y + h))
            if args.auto:
                label, _ = classify_roi(crop, args.off_v, args.red_margin, args.white_min)
                if label is None:
                    label = UNSURE_DIR
            else:
                label = args.label
            save(label, crop)
            tally[label] = tally.get(label, 0) + 1
            saved += 1
            if saved % 25 == 0:
                print(f"  {saved}/{args.count}  {tally}")
        except Exception as e:  # transient network hiccups shouldn't abort
            print(f"  warn: {e}")
        time.sleep(args.interval)
    print(f"done: {tally}")
    if args.auto:
        print("Now run:  python review.py   to fix any mislabels "
              f"(check the '{UNSURE_DIR}' folder first).")


if __name__ == "__main__":
    main()
