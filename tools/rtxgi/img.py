"""Images du banc RTXGI : lecture des PFM (HDR linéaire), courbe de tons de l'exemple (Reinhard
sur la luminance, puis sRGB), PSNR et ꟻLIP-like simples, planches et vidéos."""

import subprocess

import numpy as np


def read_pfm(path):
    with open(path, 'rb') as f:
        assert f.readline().strip() == b'PF'
        w, h = map(int, f.readline().split())
        scale = float(f.readline())
        data = np.frombuffer(f.read(), '<f4' if scale < 0 else '>f4')
    return data.reshape(h, w, 3)[::-1].astype(np.float64)


def tonemap(hdr, exposure=1.0):
    """Comme Tonemapping.hlsl : exposition, Reinhard sur la luminance, écrêtage ; puis sRGB."""
    c = np.maximum(hdr * exposure, 0)
    lum = c @ np.array([0.299, 0.587, 0.114])
    k = np.where(lum > 0, (lum / (lum + 1)) / np.maximum(lum, 1e-12), 0)
    c = np.clip(c * k[..., None], 0, 1)
    return np.where(c <= 0.0031308, 12.92 * c, 1.055 * np.power(c, 1 / 2.4) - 0.055)


def psnr(a, b):
    """PSNR des images affichées (après courbe de tons), en dB."""
    return 10 * np.log10(1.0 / max(((tonemap(a) - tonemap(b)) ** 2).mean(), 1e-12))


def rel_mse(a, b):
    """relMSE en HDR, sans le 0,1 % de pixels les plus fautifs (pratique des articles)."""
    e = (((a - b) ** 2) / (b ** 2 + 1e-2)).mean(-1).ravel()
    e.sort()
    return float(e[: int(len(e) * 0.999)].mean())


def write_png(path, ldr):
    img = (np.clip(ldr, 0, 1) * 255 + 0.5).astype(np.uint8)
    h, w, _ = img.shape
    p = subprocess.run(['ffmpeg', '-loglevel', 'error', '-y', '-f', 'rawvideo', '-pix_fmt', 'rgb24',
                        '-s', f'{w}x{h}', '-i', '-', path], input=img.tobytes())
    assert p.returncode == 0


def error_map(a, b, gain=8.0):
    """Écart signé des images affichées, gris = 0."""
    return np.clip((tonemap(a) - tonemap(b)) * gain + 0.5, 0, 1)
