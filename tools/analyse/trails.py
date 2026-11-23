"""Trails of occlusion in a burst taken while turning the camera (before_k / after_k .ppm).
For each pair of consecutive frames: horizontal shift of the game image (before) and shift of URTI's darkening map
(1 - after / before) that realigns them best. Well-attached occlusion moves as much as the image (lag 0); trailing occlusion moves less.
Also: residual difference of the darkening map once realigned on the image's motion (what does not follow the motion).
trails.py <folder> [<folder> ...] [--png output.png]"""
import glob
import os
import sys

import numpy as np
from PIL import Image


def load(d, which):
    fs = sorted(glob.glob(os.path.join(d, which + '_*.ppm')), key=lambda s: int(s.rsplit('_', 1)[1].split('.')[0]))
    return [np.asarray(Image.open(f)).astype(np.float32).mean(-1) for f in fs]


def highpass(x):
    k = np.ones(9) / 9
    b = np.apply_along_axis(lambda r: np.convolve(r, k, 'same'), 1, x)
    b = np.apply_along_axis(lambda c: np.convolve(c, k, 'same'), 0, b)
    return x - b


def best_shift(a, b, rng=60):
    # shift s such that a[:, x] ~ b[:, x - s]
    h, w = a.shape
    best, bs = -1e30, 0
    for s in range(-rng, rng + 1):
        if s >= 0:
            x, y = a[:, s:], b[:, :w - s]
        else:
            x, y = a[:, :w + s], b[:, -s:]
        x = x[:, 8:-8]
        y = y[:, 8:-8]
        den = np.sqrt((x * x).sum() * (y * y).sum()) + 1e-9
        c = (x * y).sum() / den
        if c > best:
            best, bs = c, s
    return bs, best


args = [a for a in sys.argv[1:] if not a.startswith('--')]
png = sys.argv[sys.argv.index('--png') + 1] if '--png' in sys.argv else None
for d in args:
    if png and d == png:
        continue
    A, B = load(d, 'before'), load(d, 'after')
    if len(A) < 4:
        print(d, 'not enough frames')
        continue
    D = [np.clip(1.0 - (b + 1.0) / (a + 1.0), 0, 1) for a, b in zip(A, B)]
    lags, resid, resid0 = [], [], []
    for k in range(1, len(A)):
        si, ci = best_shift(highpass(A[k]), highpass(A[k - 1]))
        if abs(si) < 3 or ci < 0.3:
            continue
        sd, cd = best_shift(D[k] - D[k].mean(), D[k - 1] - D[k - 1].mean())
        lags.append(1.0 - sd / si)
        # previous map realigned on the image's motion: what remains is what does not follow
        h, w = D[k].shape
        if si >= 0:
            r = np.abs(D[k][:, si:] - D[k - 1][:, :w - si])
        else:
            r = np.abs(D[k][:, :w + si] - D[k - 1][:, -si:])
        resid.append(r.mean() * 100)
        resid0.append(D[k].mean() * 100)
    if lags:
        print('%-26s pairs %2d | occlusion lag %.2f (0 attached, 1 frozen) | difference after realignment %.2f %% (mean darkening %.2f %%)'
              % (os.path.basename(d), len(lags), float(np.median(lags)), float(np.mean(resid)), float(np.mean(resid0))))
    else:
        print(os.path.basename(d), 'no measurable motion')
    if png:
        k = len(D) // 2
        strip = np.concatenate([np.concatenate([A[k] / 255.0, D[k] * 4], 1), np.concatenate([A[k + 1] / 255.0, D[k + 1] * 4], 1)], 0)
        Image.fromarray((np.clip(strip, 0, 1) * 255).astype(np.uint8)).save(os.path.join(d, 'trainee.png'))
