"""Collect labelled ROI samples from a running ESP32-CAM for training.

Pulls frames from the camera's /bmp endpoint, crops the configured ROI, resizes
to the model input size (exactly matching the firmware preprocessing) and saves
them under ml/data/<label>/. Run it once per LED state while holding the scanner
in that state.

Examples:
    python collect.py --host 192.168.1.50 --label off   --count 200
    python collect.py --host 192.168.1.50 --label red   --count 200
    python collect.py --host 192.168.1.50 --label white --count 200

The ROI defaults match config.h; override with --roi x y w h if you changed it
via /detcfg. Query the camera's /detcfg to see the live ROI.
"""
import argparse
import io
import os
import time

import requests
from PIL import Image

from model import INPUT_W, INPUT_H, CLASS_NAMES

HERE = os.path.dirname(os.path.abspath(__file__))

# Defaults mirror CameraWebServer/config.h.
DEFAULT_ROI = (16, 40, 128, 40)


def fetch_bmp(host):
    r = requests.get(f"http://{host}/bmp", timeout=5)
    r.raise_for_status()
    return Image.open(io.BytesIO(r.content)).convert("RGB")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", required=True, help="camera IP or host")
    ap.add_argument("--label", required=True, choices=CLASS_NAMES)
    ap.add_argument("--count", type=int, default=200)
    ap.add_argument("--interval", type=float, default=0.1, help="seconds between grabs")
    ap.add_argument("--roi", type=int, nargs=4, metavar=("X", "Y", "W", "H"),
                    default=DEFAULT_ROI)
    args = ap.parse_args()

    out_dir = os.path.join(HERE, "data", args.label)
    os.makedirs(out_dir, exist_ok=True)
    x, y, w, h = args.roi
    existing = len(os.listdir(out_dir))

    print(f"Collecting {args.count} '{args.label}' samples from {args.host} "
          f"ROI=({x},{y},{w},{h}) -> {out_dir}")
    saved = 0
    while saved < args.count:
        try:
            img = fetch_bmp(args.host)
            roi = img.crop((x, y, x + w, y + h)).resize((INPUT_W, INPUT_H))
            roi.save(os.path.join(out_dir, f"{existing + saved:05d}.png"))
            saved += 1
            if saved % 20 == 0:
                print(f"  {saved}/{args.count}")
        except Exception as e:  # transient network hiccups shouldn't abort
            print(f"  warn: {e}")
        time.sleep(args.interval)
    print("done")


if __name__ == "__main__":
    main()
