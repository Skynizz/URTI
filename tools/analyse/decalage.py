"""Analyse précise de l'occultation d'URTI dans une rafale (avant_k / apres_k .ppm : la même image du jeu sans et avec URTI).
Carte d'assombrissement D = 1 - apres / avant (ce qu'URTI retire), image du jeu A.
Mesures, au dixième de pixel (corrélation de phase, sommet affiné par parabole) :
  - mouvement de l'image d'une image à la suivante (A_k-1 -> A_k) et mouvement de la carte D (D_k-1 -> D_k) ;
    suivi = composante du mouvement de D le long de celui de l'image / mouvement de l'image (1 : accrochée, 0 : figée) ;
  - décalage fixe de D par rapport à l'image : corrélation des contours de D (gradient) avec ceux de A dans la même image
    (l'occultation se pose sur les arêtes et les contacts : bien posée, ses contours tombent sur ceux de la géométrie) ;
  - stabilité : écart moyen de D_k avec D_k-1 recalée sur le mouvement de l'image (sous-pixel), rapporté à D moyenne ;
  - force : part des pixels assombris de plus de 5 % et de 15 %.
decalage.py <dossier> [...]"""
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
    """décalage (dy, dx) tel que a(x) ~ b(x - d), sous-pixel"""
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
    """x décalée de (dy, dx) (sous-pixel, bilinéaire), bords répétés"""
    h, w = x.shape
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float64)
    sy, sx = np.clip(yy - dy, 0, h - 1), np.clip(xx - dx, 0, w - 1)
    y0, x0 = np.floor(sy).astype(int), np.floor(sx).astype(int)
    y1, x1 = np.minimum(y0 + 1, h - 1), np.minimum(x0 + 1, w - 1)
    fy, fx = sy - y0, sx - x0
    return (x[y0, x0] * (1 - fy) * (1 - fx) + x[y0, x1] * (1 - fy) * fx + x[y1, x0] * fy * (1 - fx) + x[y1, x1] * fy * fx)


for d in sys.argv[1:]:
    A, B = load(d, 'avant'), load(d, 'apres')
    if len(A) < 4:
        print(d, ': pas assez d images')
        continue
    D = [np.clip(1.0 - (b + 1.0) / (a + 1.0), 0, 1) for a, b in zip(A, B)]
    m = 12  # marge (bords, mini-carte)
    crop = (slice(m, -m), slice(m, -m))
    follow, offs, stab, img_mv = [], [], [], []
    for k in range(len(A)):
        # décalage fixe : contours de D contre contours de A dans la même image
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
    print('%-22s mouvement image %5.1f px | suivi %s | décalage fixe (y, x) %+.2f %+.2f px | instabilité %.2f | assombri >5 %% : %.1f %%, >15 %% : %.1f %%'
          % (os.path.basename(d), float(np.median(img_mv)), ('%.2f' % float(np.median(follow))) if follow else '  - ', o[0], o[1],
             float(np.median(stab)), s5, s15))
