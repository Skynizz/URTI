"""Precise analysis of URTI's darkening in a burst (before_k / after_k .ppm: the same game frame without and with URTI).
Darkening map D = 1 - after / before (what URTI removes), game image A.
Measurements, to a tenth of a pixel (phase correlation, peak refined by a parabola):
  - motion of the image from one frame to the next (A_k-1 -> A_k) and motion of the map D (D_k-1 -> D_k);
    follow = component of D's motion along the image's / the image's motion (1: attached, 0: frozen);
  - fixed offset of D relative to the image: correlation of D's edges (gradient) with A's in the same frame
    (occlusion sits on edges and contacts: when well placed, its edges fall on the geometry's);
  - stability: mean difference of D_k from D_k-1 realigned on the image's motion (sub-pixel), relative to the mean D;
  - strength: share of pixels darkened by more than 5 % and 15 %.
alignment.py <folder> [...]"""
import glob
import os
import sys

import numpy as np
from PIL import Image


def load(d, which):
    fs = sorted(glob.glob(os.path.join(d, which + '_*.ppm')), key=lambda s: int(s.rsplit('_', 1)[1].split('.')[0]))
    return [np.asarray(Image.open(f)).astype(np.float64).mean(-1) for f in fs]


def window(shape):
    h, w = shape
    return np.outer(np.hanning(h), np.hanning(w))


def phase_shift(a, b):
    """offset (dy, dx) such that a(x) ~ b(x - d), sub-pixel"""
    win = window(a.shape)
    fa, fb = np.fft.fft2((a - a.mean()) * win), np.fft.fft2((b - b.mean()) * win)
    r = fa * np.conj(fb)
    r /= np.abs(r) + 1e-9
    c = np.real(np.fft.ifft2(r))
    h, w = c.shape
    iy, ix = np.unravel_index(np.argmax(c), c.shape)

    def sub(cm, c0, cp):
        den = cm - 2 * c0 + cp
        return 0.0 if abs(den) < 1e-12 else 0.5 * (cm - cp) / den
    dy = iy + sub(c[(iy - 1) % h, ix], c[iy, ix], c[(iy + 1) % h, ix])
    dx = ix + sub(c[iy, (ix - 1) % w], c[iy, ix], c[iy, (ix + 1) % w])
    if dy > h / 2:
        dy -= h
    if dx > w / 2:
        dx -= w
    return dy, dx, float(c.max())


def grad(x):
    gx = np.zeros_like(x)
    gy = np.zeros_like(x)
    gx[:, 1:-1] = x[:, 2:] - x[:, :-2]
    gy[1:-1] = x[2:] - x[:-2]
    return np.hypot(gx, gy)


def shift_img(x, dy, dx):
    """x shifted by (dy, dx) (sub-pixel, bilinear), edges repeated"""
    h, w = x.shape
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float64)
    sy, sx = np.clip(yy - dy, 0, h - 1), np.clip(xx - dx, 0, w - 1)
    y0, x0 = np.floor(sy).astype(int), np.floor(sx).astype(int)
    y1, x1 = np.minimum(y0 + 1, h - 1), np.minimum(x0 + 1, w - 1)
    fy, fx = sy - y0, sx - x0
    return (x[y0, x0] * (1 - fy) * (1 - fx) + x[y0, x1] * (1 - fy) * fx + x[y1, x0] * fy * (1 - fx) + x[y1, x1] * fy * fx)


for d in sys.argv[1:]:
    A, B = load(d, 'before'), load(d, 'after')
    if len(A) < 4:
        print(d, ': not enough frames')
        continue
    D = [np.clip(1.0 - (b + 1.0) / (a + 1.0), 0, 1) for a, b in zip(A, B)]
    m = 12  # margin (edges, minimap)
    crop = (slice(m, -m), slice(m, -m))
    follow, offs, stab, img_mv = [], [], [], []
    for k in range(len(A)):
        # fixed offset: D's edges against A's edges in the same frame
        ga, gd = grad(A[k])[crop], grad(D[k])[crop]
        if gd.std() > 1e-4:
            oy, ox, _ = phase_shift(gd, ga)
            if abs(oy) < 6 and abs(ox) < 6:
                offs.append((oy, ox))
        if k == 0:
            continue
        iy, ix, ci = phase_shift(A[k][crop], A[k - 1][crop])
        dy, dx, cd = phase_shift(D[k][crop], D[k - 1][crop])
        mv = np.hypot(iy, ix)
        img_mv.append(mv)
        if mv >= 1.0:
            follow.append((dy * iy + dx * ix) / (mv * mv))
        prev = shift_img(D[k - 1], iy, ix)
        stab.append(np.abs(D[k] - prev)[crop].mean() / max(D[k][crop].mean(), 1e-4))
    s5 = np.mean([(Dk > 0.05).mean() for Dk in D]) * 100
    s15 = np.mean([(Dk > 0.15).mean() for Dk in D]) * 100
    o = np.median(np.array(offs), 0) if offs else (float('nan'), float('nan'))
    print('%-22s image motion %5.1f px | follow %s | fixed offset (y, x) %+.2f %+.2f px | instability %.2f | darkened >5 %% : %.1f %%, >15 %% : %.1f %%'
          % (os.path.basename(d), float(np.median(img_mv)), ('%.2f' % float(np.median(follow))) if follow else '  - ', o[0], o[1],
             float(np.median(stab)), s5, s15))
