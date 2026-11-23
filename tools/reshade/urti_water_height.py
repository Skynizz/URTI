"""URTI: height of water pixels (film: depth.bin, material.bin, FILMCAM line of the log).

    python urti_water_height.py <film folder> <log>

The reflection starts from the point the game's depth gives. If the depth is the water surface, every water pixel is at the same
height (a plane). If it is the bottom (shallow transparent water), their height follows the bottom: the mirror is too low and bumpy, the
reflection shifts and wobbles when the camera moves. Output: water_height.png (height of the water pixels, from the lowest in blue to the highest
in red; grey: not water).
"""
import os
import re
import sys

import numpy as np
from PIL import Image

d, journal = sys.argv[1], sys.argv[2]
cam = None
for line in open(journal, encoding='utf-8', errors='replace'):
    if line.startswith('FILMCAM'):
        cam = line
m = re.search(r'A (\S+) B (\S+) p00 (\S+) p11 (\S+) up (\S+) (\S+) (\S+) d24 (\d) pitch (\d+) render (\d+)x(\d+) alloc (\d+)x(\d+) '
              r'material (\d+) (\d+)x(\d+) format (\d+) channel (\d+) inverted (\d)', cam)
A, B, p00, p11, ux, uy, uz = (float(m[i]) for i in range(1, 8))
d24, pitch, rw, rh, aw, ah, mpitch, mw, mh, mfmt, chan, inv = (int(m[i]) for i in range(8, 20))
raw = np.frombuffer(open(os.path.join(d, 'depth.bin'), 'rb').read(), dtype=np.uint32)
raw = raw[:pitch // 4 * ah].reshape(ah, pitch // 4)[:rh, :rw]
dep = (raw & 0xFFFFFF).astype(np.float64) / 16777215.0 if d24 else raw.view(np.float32).astype(np.float64)
w = B / (dep - A)
ys, xs = np.mgrid[0:rh, 0:rw]
nx = (xs + 0.5) / rw * 2 - 1
ny = 1 - (ys + 0.5) / rh * 2
P = np.stack([nx * w / p00, ny * w / p11, w], -1)
height = P @ np.array([ux, uy, uz])
mat = np.frombuffer(open(os.path.join(d, 'material.bin'), 'rb').read(), dtype=np.uint8)
mat = mat[:mpitch * mh].reshape(mh, mpitch)[:rh, :rw * 4].reshape(rh, rw, 4)[..., chan].astype(np.float64) / 255.0
water = (mat <= 0.1) if inv else (mat >= 0.9)  # smoothness at least 0.9 (smooth_min)
valid = np.isfinite(height) & (w > 0) & (w < 2000)
wv = water & valid
print(f'render {rw}x{rh}, water pixels {wv.mean() * 100:.1f} %')
if wv.any():
    h = height[wv]
    dist = w[wv]
    print(f'water height below the camera: median {np.median(h):.3f} m, 5-95 % {np.percentile(h, 5):.3f} to {np.percentile(h, 95):.3f} m '
          f'(spread {np.percentile(h, 95) - np.percentile(h, 5):.3f} m), median distance {np.median(dist):.1f} m')
    # surface: the highest water pixels at the water's edge (where the bottom meets the surface)
    edge = water & ~np.roll(water, 1, 0) | water & ~np.roll(water, -1, 0) | water & ~np.roll(water, 1, 1) | water & ~np.roll(water, -1, 1)
    he = height[edge & valid]
    if he.size:
        print(f'water edge: median height {np.median(he):.3f} m (90 %: {np.percentile(he, 90):.3f} m); '
              f'water pixels more than 10 cm below that level: {(h < np.percentile(he, 90) - 0.10).mean() * 100:.0f} %')
    lo, hi = np.percentile(h, 2), np.percentile(h, 98)
    t = np.clip((height - lo) / max(hi - lo, 1e-3), 0, 1)
    img = np.stack([t, 0.3 + 0 * t, 1 - t], -1)
    img[~wv] = 0.35
    Image.fromarray((img * 255).astype(np.uint8)).save(os.path.join(d, 'water_height.png'))
