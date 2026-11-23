"""URTI: bright points added by URTI, on each burst (urti_burst_N, before / after) of a folder.

    python urti_sparkles_ana.py <folder holding the bursts>

Columns: share of pixels brightened by more than 5, 10, 20 %, darkened by more than 10 %; isolated points brighter than their neighbourhood added by URTI (count,
pixels), those already in the game's image; change of the effect from one frame to the next. To find which effect makes them, turn off reflect=, gi=, ao= in
urti.ini between two bursts (hot-reloaded). Output in each burst: bright/red_added.png (what URTI brightens, in red).
"""
import sys, re, os
import numpy as np
from scipy.ndimage import median_filter, label
from PIL import Image


def load(p):
    d = open(p, 'rb').read()
    m = re.match(rb'P6\s+(\d+)\s+(\d+)\s+(\d+)\s', d)
    w, h = int(m[1]), int(m[2])
    return np.frombuffer(d[m.end():m.end() + w * h * 3], np.uint8).reshape(h, w, 3).astype(np.float32) / 255


root = sys.argv[1]
print(f"{'burst':24} {'+>.05':>7} {'+>.1':>7} {'+>.2':>7} {'->.1':>7} | {'points':>6} {'pix':>6} | {'game pts':>8} | {'|dC|>.05':>8} {'|dC|>.1':>8}")
for name in sorted(os.listdir(root)):
    f = os.path.join(root, name)
    if not os.path.isdir(f) or not os.path.exists(f + '/after_0.ppm'):
        continue
    idx = sorted(int(m[1]) for n in os.listdir(f) if (m := re.match(r'after_(\d+)\.ppm$', n)))
    A = np.stack([load(f'{f}/before_{i}.ppm').mean(2) for i in idx])
    B = np.stack([load(f'{f}/after_{i}.ppm').mean(2) for i in idx])
    C = B - A
    pos = np.clip(C, 0, 1)
    neg = np.clip(-C, 0, 1)
    pts = []
    game = []
    # one frame in three: bright points brighter than their neighbourhood, added by URTI / already in the game's image
    for k in range(0, len(idx), 3):
        s = (B[k] - median_filter(B[k], size=9) > 0.12) & (pos[k] > 0.08)
        lab, n = label(s)
        pts.append((n, int(s.sum())))
        g = A[k] - median_filter(A[k], size=9) > 0.12
        game.append(label(g)[1])
    dC = np.abs(C[1:] - C[:-1])
    print(f"{name:24} {100*(pos>.05).mean():7.3f} {100*(pos>.1).mean():7.3f} {100*(pos>.2).mean():7.3f} {100*(neg>.1).mean():7.3f} | "
          f"{np.mean([p[0] for p in pts]):6.1f} {np.mean([p[1] for p in pts]):6.1f} | {np.mean(game):8.1f} | {100*(dC>.05).mean():8.3f} {100*(dC>.1).mean():8.3f}")
    out = f + '/bright'
    os.makedirs(out, exist_ok=True)
    g8 = lambda x: (np.clip(x, 0, 1) * 255).astype(np.uint8)
    k = len(idx) // 2
    Image.fromarray(np.stack([g8(B[k] * 0.6 + pos[k] * 4), g8(B[k] * 0.6), g8(B[k] * 0.6)], 2)).save(out + '/red_added.png')
    Image.fromarray(g8(B[k])).save(out + '/after.png')
    Image.fromarray(g8(A[k])).save(out + '/before.png')
