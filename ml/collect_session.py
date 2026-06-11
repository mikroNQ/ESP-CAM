"""Session-based ROI collector: one fixed label per run, gated by brightness.

Why not `collect.py --auto`? The colour heuristic doesn't separate red/white on
this rig — the lit LED band reads near-neutral (R≈G≈B), so every crop lands in
`_unsure`. But ON vs OFF is unambiguous by ROI brightness, and the scanner
*type* (red/white) is known per session. So: hold ONE scanner state and run this
with its label. Frames are gated by ROI mean brightness, so a brief illumination
dropout in an ON session (or a stray flash in an OFF session) is skipped instead
of being mislabelled.

Frames come from /capture (JPEG, ~0.5s each) rather than /bmp (RGB BMP, ~2.5s
on a no-PSRAM board) — far faster, and the mild JPEG artefacts are harmless for
training. ROI is read live from the camera's /detcfg, so it always matches the
firmware. Crops are saved at native ROI resolution into ml/data/<label>/ —
train.py resizes them to the model input (Image.BOX) to match firmware preproc.

Examples:
    python collect_session.py --host 172.27.165.190 --label white_on --min-v 80  --count 300
    python collect_session.py --host 172.27.165.190 --label off     --max-v 60  --count 300
    python collect_session.py --host 172.27.165.190 --label red_on   --min-v 80  --count 300
"""
import argparse
import io
import os
import time

import numpy as np
import requests
from PIL import Image

from dataset_util import next_index

HERE = os.path.dirname(os.path.abspath(__file__))


def fetch_frame(host):
    # /capture (JPEG) is ~5x faster than /bmp on no-PSRAM boards.
    r = requests.get(f"http://{host}/capture", timeout=10)
    r.raise_for_status()
    return Image.open(io.BytesIO(r.content)).convert("RGB")


def get_roi(host):
    r = requests.get(f"http://{host}/detcfg", timeout=5).json()["roi"]
    return r["x"], r["y"], r["w"], r["h"]


def apply_exposure_lock(host, aec_value):
    """Force fixed exposure/gain/white-balance. The firmware re-inits the sensor
    to auto after a camera stall, so callers re-assert this periodically."""
    for var, val in [("awb", 0), ("awb_gain", 0), ("aec", 0), ("aec2", 0),
                     ("agc", 0), ("agc_gain", 0), ("aec_value", aec_value)]:
        try:
            requests.get(f"http://{host}/control", params={"var": var, "val": val}, timeout=5)
        except Exception:
            pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", required=True, help="camera IP or host")
    ap.add_argument("--label", required=True, help="folder/label for every saved crop")
    ap.add_argument("--count", type=int, default=300)
    ap.add_argument("--interval", type=float, default=0.1, help="seconds between grabs")
    ap.add_argument("--min-v", type=float, default=None,
                    help="skip frames with ROI mean brightness below this (use for ON labels)")
    ap.add_argument("--max-v", type=float, default=None,
                    help="skip frames with ROI mean brightness above this (use for off)")
    ap.add_argument("--max-seconds", type=float, default=120.0,
                    help="wall-clock safety cap so a dark scene can't spin forever")
    ap.add_argument("--roi", type=int, nargs=4, default=None, metavar=("X", "Y", "W", "H"),
                    help="override ROI (default: read live from /detcfg)")
    ap.add_argument("--lock-aec", type=int, default=None, metavar="AEC_VALUE",
                    help="hold fixed exposure at this aec_value, re-asserting it "
                         "periodically (the firmware reverts to auto after a stall)")
    args = ap.parse_args()

    if args.lock_aec is not None:
        apply_exposure_lock(args.host, args.lock_aec)
    x, y, w, h = tuple(args.roi) if args.roi else get_roi(args.host)
    out_dir = os.path.join(HERE, "data", args.label)
    idx = next_index(out_dir)
    print(f"Collecting up to {args.count} '{args.label}' crops from {args.host} "
          f"ROI=({x},{y},{w},{h}) gate[min={args.min_v} max={args.max_v}]")

    saved = 0
    skipped = 0
    iters = 0
    start = time.time()
    while saved < args.count and (time.time() - start) < args.max_seconds:
        iters += 1
        # Re-assert the exposure lock periodically: a camera stall reverts the
        # sensor to auto, which would brighten 'off' and blur the dataset.
        if args.lock_aec is not None and iters % 30 == 0:
            apply_exposure_lock(args.host, args.lock_aec)
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
        print("  NOTE: nothing saved — was the scanner in the expected state? "
              "check the --min-v/--max-v gate vs the live ROI brightness.")


if __name__ == "__main__":
    main()
