#!/usr/bin/env python3
"""
diff.py OUT.png DIR_A DIR_B SECONDS...: a contact sheet of the screenshots of two runs of tools/look/run.sh (row 1 DIR_A,
row 2 DIR_B), the one of each directory taken nearest to each of SECONDS after the run began, and the mean absolute
difference of the two pictures of each pair (0-255, over all channels) on stderr.
Needs PIL and numpy (nix-shell -p python3Packages.pillow python3Packages.numpy).
"""
import os
import sys

import numpy as np
from PIL import Image

W = 360


def nearest(directory, seconds):
    shots = [(abs(int(f.split('-')[1][:-4]) - seconds), f) for f in os.listdir(directory) if f.endswith('.png')]
    return os.path.join(directory, min(shots)[1])


def main():
    if len(sys.argv) < 5:
        sys.exit(__doc__)
    out, a, b, times = sys.argv[1], sys.argv[2], sys.argv[3], [int(t) for t in sys.argv[4:]]
    sheet = None
    for col, t in enumerate(times):
        pics = [Image.open(nearest(d, t)).convert('RGB') for d in (a, b)]
        h = pics[0].height * W // pics[0].width
        if sheet is None:
            sheet = Image.new('RGB', (W * len(times), 2 * h))
        for row, pic in enumerate(pics):
            sheet.paste(pic.resize((W, h), Image.LANCZOS), (col * W, row * h))
        diff = np.abs(np.asarray(pics[0], dtype=np.int16) - np.asarray(pics[1], dtype=np.int16)).mean()
        print(f'{t:4d} s: mean difference {diff:.2f}', file=sys.stderr)
    sheet.save(out)


if __name__ == '__main__':
    main()
