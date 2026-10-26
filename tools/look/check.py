#!/usr/bin/env python3
"""
check.py KIND PNG: a number-based check of one screenshot (1080 x 2400, as `xl screenshot` takes it).

  pods PNG      the App Library standing: the empty part of a pod (the Health & Fitness pod, below its two icons) is
                lighter than the wallpaper beside it. The pod's matrix result is a light colour of alpha about 0.4,
                which has to be laid OVER the wallpaper; written in place of it (the fault of 2026-10-25, a mix
                where an over was needed) the pod came out darker than the wallpaper, 113 against 122.
  lit PNG       the screen shows something (not black: mean luma over 20)
  --self-test

Needs numpy and PIL (nix-shell -p python3Packages.pillow python3Packages.numpy). Exit 0 when the check holds.
"""
import sys

import numpy as np

POD = (800, 980, 1300, 1420)        # x0, x1, y0, y1: the Health & Fitness pod's empty part, library at rest
GUTTER = (522, 556, 1300, 1420)     # the wallpaper between the two columns of pods, same rows
MARGIN = 15                         # levels (of 255) the pod has to be lighter by; a flat dark pod is 9 darker
LUMA = np.array([0.299, 0.587, 0.114])


def luma(image, box):
    x0, x1, y0, y1 = box
    return float((image[y0:y1, x0:x1].reshape(-1, 3).mean(0) * LUMA).sum())


def pods(image):
    pod, gutter = luma(image, POD), luma(image, GUTTER)
    return pod - gutter >= MARGIN, f'pod luma {pod:.0f}, wallpaper beside it {gutter:.0f}, need pod >= wallpaper + {MARGIN}'


def lit(image):
    mean = float((image.reshape(-1, 3).mean(0) * LUMA).sum())
    return mean > 20, f'mean luma {mean:.0f}'


def load(path):
    from PIL import Image
    return np.asarray(Image.open(path).convert('RGB'), dtype=float)


def self_test():
    flat = np.full((2400, 1080, 3), 120.0)
    dark = flat.copy(); dark[1300:1420, 800:980] = 100          # the old fault: a pod darker than its wallpaper
    good = flat.copy(); good[1300:1420, 800:980] = 160
    assert not pods(dark)[0] and pods(good)[0] and not pods(flat)[0]
    assert lit(flat)[0] and not lit(np.zeros((2400, 1080, 3)))[0]
    print('check.py: self-test ok')


def main():
    if sys.argv[1:] == ['--self-test']:
        return self_test()
    if len(sys.argv) != 3 or sys.argv[1] not in ('pods', 'lit'):
        sys.exit(__doc__)
    ok, why = {'pods': pods, 'lit': lit}[sys.argv[1]](load(sys.argv[2]))
    print(f'check {sys.argv[1]}: {"PASS" if ok else "FAIL"}: {why}')
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
