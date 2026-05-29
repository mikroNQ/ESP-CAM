"""Review/fix auto-labelled ROI crops before training.

Walks ml/data/{_unsure,off,red,white}, shows each crop enlarged with its current
folder label and the heuristic's suggestion, and lets you correct it fast.

Keys:
    o / r / w   move crop to off / red / white
    d           delete crop
    Right / n   next        Left / p   previous
    q / Esc     quit

The _unsure pile is shown first — those are the ones the heuristic wasn't sure
about. Run after collect.py --auto.
"""
import os
import sys

from PIL import Image

from model import CLASS_NAMES
from autolabel import classify_roi

HERE = os.path.dirname(os.path.abspath(__file__))
DATA_DIR = os.path.join(HERE, "data")
UNSURE_DIR = "_unsure"
FOLDERS = [UNSURE_DIR] + CLASS_NAMES
KEY_TO_LABEL = {"o": "off", "r": "red", "w": "white"}


def list_items():
    items = []
    for folder in FOLDERS:
        d = os.path.join(DATA_DIR, folder)
        if not os.path.isdir(d):
            continue
        for f in sorted(os.listdir(d)):
            if f.endswith(".png"):
                items.append([os.path.join(d, f), folder])
    return items


def next_index(out_dir):
    os.makedirs(out_dir, exist_ok=True)
    return len([f for f in os.listdir(out_dir) if f.endswith(".png")])


def main():
    try:
        import tkinter as tk
        from PIL import ImageTk
    except Exception as e:
        sys.exit(f"Tkinter/Pillow with Tk support required for the GUI: {e}")

    items = list_items()
    if not items:
        sys.exit("No crops found in ml/data/. Run collect.py --auto first.")

    state = {"i": 0}
    root = tk.Tk()
    root.title("LED dataset review")
    img_label = tk.Label(root)
    img_label.pack()
    status = tk.Label(root, font=("TkDefaultFont", 12), justify="left")
    status.pack(fill="x")
    help_txt = tk.Label(root, fg="gray",
                        text="o/r/w relabel   d delete   ←/→ navigate   q quit")
    help_txt.pack(fill="x")

    def show():
        if not items:
            status.config(text="All crops handled. Press q to quit.")
            img_label.config(image="")
            return
        state["i"] %= len(items)
        path, folder = items[state["i"]]
        try:
            im = Image.open(path).convert("RGB")
        except Exception as e:
            status.config(text=f"cannot open {path}: {e}")
            return
        suggest, st = classify_roi(im)
        # Enlarge with nearest-neighbour so the tiny crop is visible.
        scale = max(1, min(12, 480 // max(im.width, 1)))
        disp = im.resize((im.width * scale, im.height * scale), Image.NEAREST)
        tkimg = ImageTk.PhotoImage(disp)
        img_label.config(image=tkimg)
        img_label.image = tkimg  # keep ref
        status.config(text=(
            f"[{state['i'] + 1}/{len(items)}]  folder='{folder}'  "
            f"suggest={suggest or 'unsure'}\n"
            f"{os.path.basename(path)}   "
            f"R={st['r']:.0f} G={st['g']:.0f} B={st['b']:.0f} "
            f"V={st['v']:.0f} redness={st['redness']:.0f}"))

    def relabel(label):
        if not items:
            return
        path, folder = items[state["i"]]
        if folder != label:
            out_dir = os.path.join(DATA_DIR, label)
            dst = os.path.join(out_dir, f"{next_index(out_dir):05d}.png")
            os.replace(path, dst)
        else:
            items.pop(state["i"])  # already correct; just advance
            show()
            return
        items.pop(state["i"])
        show()

    def delete():
        if not items:
            return
        path, _ = items[state["i"]]
        try:
            os.remove(path)
        except OSError:
            pass
        items.pop(state["i"])
        show()

    def nav(d):
        if items:
            state["i"] = (state["i"] + d) % len(items)
            show()

    def on_key(e):
        k = e.keysym.lower()
        if k in ("q", "escape"):
            root.destroy()
        elif k in KEY_TO_LABEL:
            relabel(KEY_TO_LABEL[k])
        elif k == "d":
            delete()
        elif k in ("right", "n"):
            nav(1)
        elif k in ("left", "p"):
            nav(-1)

    root.bind("<Key>", on_key)
    show()
    root.mainloop()
    print("Counts after review:")
    for folder in CLASS_NAMES:
        d = os.path.join(DATA_DIR, folder)
        n = len([f for f in os.listdir(d) if f.endswith(".png")]) if os.path.isdir(d) else 0
        print(f"  {folder}: {n}")


if __name__ == "__main__":
    main()
