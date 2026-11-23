"""URTI: does the effect follow the scenery when the camera moves? (burst taken during a camera move)

    python urti_burst_alignment.py <urti_burst_N folder>

For each pair of consecutive frames: shift of the game image (phase correlation on the image without
URTI) and shift of URTI's contribution (after - before). An effect attached to its objects shifts as much as
the image (follow 100 %); a trailing effect shifts less (history badly realigned); stuck to the screen: 0 %.
"""
import os
import re
import sys

import numpy as np


def load(path):
    with open(path, 'rb') as f:
        data = f.read()
    m = re.match(rb'P6\s+(\d+)\s+(\d+)\s+(\d+)\s', data)
    w, h = int(m.group(1)), int(m.group(2))
    return np.frombuffer(data[m.end():m.end() + w * h * 3], np.uint8).reshape(h, w, 3).astype(np.float32) / 255.0


def shift(a, b):
    """shift (dx, dy) that brings a onto b, to a tenth of a pixel (phase correlation, refined peak)"""
    win = np.outer(np.hanning(a.shape[0]), np.hanning(a.shape[1]))
    fa, fb = np.fft.fft2((a - a.mean()) * win), np.fft.fft2((b - b.mean()) * win)
    r = fb * np.conj(fa)
    r /= np.abs(r) + 1e-9
    c = np.fft.ifft2(r).real
    y, x = np.unravel_index(np.argmax(c), c.shape)

    def refine(cm, c0, cp):
        d = cm - 2 * c0 + cp
        return 0.0 if abs(d) < 1e-12 else 0.5 * (cm - cp) / d
    h, w = c.shape
    dy = y + refine(c[(y - 1) % h, x], c[y, x], c[(y + 1) % h, x])
    dx = x + refine(c[y, (x - 1) % w], c[y, x], c[y, (x + 1) % w])
    if dy > h / 2:
        dy -= h
    if dx > w / 2:
        dx -= w
    return dx, dy


def main():
    folder = sys.argv[1]
    idx = sorted(int(m.group(1)) for n in os.listdir(folder) if (m := re.match(r'after_(\d+)\.ppm$', n)))
    lum = np.array([0.2126, 0.7152, 0.0722], np.float32)
    rows = []
    for k in range(1, len(idx)):
        a0, a1 = load(os.path.join(folder, f'before_{idx[k - 1]}.ppm')), load(os.path.join(folder, f'before_{idx[k]}.ppm'))
        b0, b1 = load(os.path.join(folder, f'after_{idx[k - 1]}.ppm')), load(os.path.join(folder, f'after_{idx[k]}.ppm'))
        # upper half of the image, outside the UI: distant scenery shifts as a block when the camera turns
        h = a0.shape[0]
        crop = slice(0, int(h * 0.6))
        g0, g1 = (a0 @ lum)[crop], (a1 @ lum)[crop]
        e0, e1 = ((a0 - b0) @ lum)[crop], ((a1 - b1) @ lum)[crop]
        sx, sy = shift(g0, g1)
        ex, ey = shift(e0, e1)
        n = np.hypot(sx, sy)
        if n < 1.0 or float(np.abs(e1).mean()) < 1e-3:
            continue
        follow = (ex * sx + ey * sy) / (n * n)
        rows.append((idx[k], sx, sy, ex, ey, follow))
    if not rows:
        sys.exit('not enough motion or effect to measure')
    for i, sx, sy, ex, ey, f in rows:
        print(f'{i:3d}  image {sx:6.1f} {sy:6.1f} px   effet {ex:6.1f} {ey:6.1f} px   suivi {100 * f:5.0f} %')
    print(f'median follow: {100 * float(np.median([r[5] for r in rows])):.0f} % (100 %: the effect stays attached to its objects)')


if __name__ == '__main__':
    main()
