"""URTI : hauteur des pixels d'eau (film : profondeur.bin, materiau.bin, ligne FILMCAM du journal).

    python urti_water_height.py <dossier du film> <journal>

Le reflet part du point que donne la profondeur du jeu. Si la profondeur est la surface de l'eau, tous les pixels d'eau sont à la même
hauteur (un plan). Si c'est le fond (eau transparente peu profonde), leur hauteur suit le fond : le miroir est trop bas et bosselé, le
reflet se décale et ondule quand la caméra bouge. Sortie : hauteur_eau.png (hauteur des pixels d'eau, du plus bas en bleu au plus haut
en rouge ; gris : pas de l'eau).
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
m = re.search(r'A (\S+) B (\S+) p00 (\S+) p11 (\S+) haut (\S+) (\S+) (\S+) d24 (\d) pas (\d+) rendu (\d+)x(\d+) alloue (\d+)x(\d+) '
              r'materiau (\d+) (\d+)x(\d+) format (\d+) canal (\d+) inverse (\d)', cam)
A, B, p00, p11, ux, uy, uz = (float(m[i]) for i in range(1, 8))
d24, pitch, rw, rh, aw, ah, mpitch, mw, mh, mfmt, chan, inv = (int(m[i]) for i in range(8, 20))
raw = np.frombuffer(open(os.path.join(d, 'profondeur.bin'), 'rb').read(), dtype=np.uint32)
raw = raw[:pitch // 4 * ah].reshape(ah, pitch // 4)[:rh, :rw]
dep = (raw & 0xFFFFFF).astype(np.float64) / 16777215.0 if d24 else raw.view(np.float32).astype(np.float64)
w = B / (dep - A)
ys, xs = np.mgrid[0:rh, 0:rw]
nx = (xs + 0.5) / rw * 2 - 1
ny = 1 - (ys + 0.5) / rh * 2
P = np.stack([nx * w / p00, ny * w / p11, w], -1)
height = P @ np.array([ux, uy, uz])
mat = np.frombuffer(open(os.path.join(d, 'materiau.bin'), 'rb').read(), dtype=np.uint8)
mat = mat[:mpitch * mh].reshape(mh, mpitch)[:rh, :rw * 4].reshape(rh, rw, 4)[..., chan].astype(np.float64) / 255.0
water = (mat <= 0.1) if inv else (mat >= 0.9)  # lissé au moins 0,9 (smooth_min)
valid = np.isfinite(height) & (w > 0) & (w < 2000)
wv = water & valid
print(f'rendu {rw}x{rh}, pixels d\'eau {wv.mean() * 100:.1f} %')
if wv.any():
    h = height[wv]
    dist = w[wv]
    print(f'hauteur de l\'eau sous la caméra : médiane {np.median(h):.3f} m, 5-95 % {np.percentile(h, 5):.3f} à {np.percentile(h, 95):.3f} m '
          f'(écart {np.percentile(h, 95) - np.percentile(h, 5):.3f} m), distance médiane {np.median(dist):.1f} m')
    # surface : les pixels d'eau les plus hauts au bord de l'eau (là où le fond rejoint la surface)
    edge = water & ~np.roll(water, 1, 0) | water & ~np.roll(water, -1, 0) | water & ~np.roll(water, 1, 1) | water & ~np.roll(water, -1, 1)
    he = height[edge & valid]
    if he.size:
        print(f'bord de l\'eau : hauteur médiane {np.median(he):.3f} m (90 % : {np.percentile(he, 90):.3f} m) ; '
              f'pixels d\'eau sous ce niveau de plus de 10 cm : {(h < np.percentile(he, 90) - 0.10).mean() * 100:.0f} %')
    lo, hi = np.percentile(h, 2), np.percentile(h, 98)
    t = np.clip((height - lo) / max(hi - lo, 1e-3), 0, 1)
    img = np.stack([t, 0.3 + 0 * t, 1 - t], -1)
    img[~wv] = 0.35
    Image.fromarray((img * 255).astype(np.uint8)).save(os.path.join(d, 'hauteur_eau.png'))
