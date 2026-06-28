"""Session-based ROI collector for the drink dataset (STAGE 2).

One fixed label per run. Hold ONE cup state (empty / black coffee / latte) under
the camera and run this with its label; it snaps the ROI crop repeatedly. An
optional brightness gate skips frames that don't match the held state (a hand
reaching in, a pour still in progress), so you don't have to babysit the timing.

Frames come from /capture (JPEG, fast). The ROI is read live from the camera's
/cupcfg, so crops always match what the firmware classifies. Crops are saved at
native ROI resolution into coffee/ml/data/<label>/ — train.py resizes them to the
model input (24x24, Image.BOX) to match the firmware preprocessing.

Exposure: lock it ONCE before collecting and use the SAME lock at deployment —
  curl "http://coffeecam.local/cupcfg?fixexp=1&aec_value=600&agc_gain=0"
The firmware holds that lock (re-asserting every ~2s), so this script doesn't
touch exposure. Verify `cup_fixexp:1` in /status before a session.

Labels are the CLASS_NAMES in model.py (edit them to your machine menu).

Examples:
    python collect_session.py --host coffeecam.local --label empty       --count 400
    python collect_session.py --host coffeecam.local --label cappuccino  --count 600
    python collect_session.py --host coffeecam.local --label tea         --count 400 --max-v 200
"""
import argparse
import io
import os
import time

import numpy as np
import requests
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))


def fetch_frame(host):
    r = requests.get(f"http://{host}/capture", timeout=10)
    r.raise_for_status()
    return Image.open(io.BytesIO(r.content)).convert("RGB")


def get_roi(host):
    # The coffee firmware exposes the ROI on /cupcfg (vs /detcfg in the parent).
    r = requests.get(f"http://{host}/cupcfg", timeout=5).json()["roi"]
    return r["x"], r["y"], r["w"], r["h"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", required=True, help="camera IP or host (e.g. coffeecam.local)")
    ap.add_argument("--label", required=True, help="folder/label for every saved crop")
    ap.add_argument("--count", type=int, default=300)
    ap.add_argument("--interval", type=float, default=0.1, help="seconds between grabs")
    ap.add_argument("--min-v", type=float, default=None,
                    help="skip frames with ROI mean brightness below this")
    ap.add_argument("--max-v", type=float, default=None,
                    help="skip frames with ROI mean brightness above this")
    ap.add_argument("--max-seconds", type=float, default=180.0,
                    help="wall-clock safety cap so a wrong gate can't spin forever")
    ap.add_argument("--roi", type=int, nargs=4, default=None, metavar=("X", "Y", "W", "H"),
                    help="override ROI (default: read live from /cupcfg)")
    args = ap.parse_args()

    x, y, w, h = tuple(args.roi) if args.roi else get_roi(args.host)
    out_dir = os.path.join(HERE, "data", args.label)
    os.makedirs(out_dir, exist_ok=True)
    idx = len([f for f in os.listdir(out_dir) if f.endswith(".png")])
    print(f"Collecting up to {args.count} '{args.label}' crops from {args.host} "
          f"ROI=({x},{y},{w},{h}) gate[min={args.min_v} max={args.max_v}]")

    saved = 0
    skipped = 0
    start = time.time()
    while saved < args.count and (time.time() - start) < args.max_seconds:
        try:
            img = fetch_frame(args.host)
            crop = img.crop((x, y, x + w, y + h))
            v = float(np.asarray(crop, dtype=np.float32).mean())
            if (args.min_v is not None and v < args.min_v) or \
               (args.max_v is not None and v > args.max_v):
                skipped += 1
                if skipped % 25 == 0:
                    print(f"  gated out {skipped} (last V={v:.0f}) — holding for the right state")
                time.sleep(args.interval)
                continue
            crop.save(os.path.join(out_dir, f"{idx:05d}.png"))
            idx += 1
            saved += 1
            if saved % 25 == 0:
                print(f"  {saved}/{args.count}  (V={v:.0f})")
        except Exception as e:  # transient network hiccups shouldn't abort
            print(f"  warn: {e}")
        time.sleep(args.interval)

    why = "count reached" if saved >= args.count else f"time cap ({args.max_seconds:.0f}s)"
    print(f"done [{why}]: saved {saved} to data/{args.label} (gated out {skipped})")
    if saved == 0:
        print("  NOTE: nothing saved — is the cup in the expected state? "
              "check --min-v/--max-v vs the live ROI brightness in /metrics.")


if __name__ == "__main__":
    main()
