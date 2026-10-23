"""Lecture des captures PPM de la DLL (KRC_CAPTURE) et mesures d'écart entre images."""

import numpy as np


def read(path):
    with open(path, 'rb') as f:
        data = f.read()
    # en-tête « P6\n<l> <h>\n255\n »
    parts = data.split(b'\n', 3)
    w, h = map(int, parts[1].split())
    return np.frombuffer(parts[3], np.uint8)[:w * h * 3].reshape(h, w, 3).astype(np.float64) / 255.0


def write(path, img):
    img = (np.clip(img, 0, 1) * 255 + 0.5).astype(np.uint8)
    with open(path, 'wb') as f:
        f.write(b'P6\n%d %d\n255\n' % (img.shape[1], img.shape[0]))
        f.write(img.tobytes())


def psnr(a, b, mask=None):
    d = (a - b) ** 2
    mse = d[mask].mean() if mask is not None else d.mean()
    return 10 * np.log10(1.0 / max(mse, 1e-12))


def luminance(a):
    return a @ np.array([0.2126, 0.7152, 0.0722])
