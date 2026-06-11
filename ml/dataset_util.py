"""Shared helpers for the ml/data sample folders."""
import os
import re

_NUM_PNG = re.compile(r"^(\d+)\.png$")


def next_index(out_dir):
    """Next free NNNNN.png index in out_dir: max existing index + 1.

    Counting files (len of listing) breaks after deletions: a hole in the
    numbering makes the count smaller than the highest index, and new saves
    silently overwrite existing samples.
    """
    os.makedirs(out_dir, exist_ok=True)
    last = -1
    for f in os.listdir(out_dir):
        m = _NUM_PNG.match(f)
        if m:
            last = max(last, int(m.group(1)))
    return last + 1
