"""URTI : analyse d'un film au rythme réel (fichier urti_film, urti_film_N\\avant_k.raw / apres_k.raw, une ligne FILM par présentation).

    python urti_film_ana.py <dossier du film> [journal] [numéro du film dans le journal]

Mesure ce que l'œil voit trembler, avec ꟻLIP (NVIDIA, flip-evaluator) : erreur perçue entre chaque présentation et la suivante, sur l'image
du jeu (avant) et sur l'image avec URTI (après). Ce que le jeu change d'une image à l'autre (mouvement de la scène) est commun aux deux ;
l'excédent après - avant est ce qu'URTI ajoute : reflet qui saute, tremble, se décale par à-coups. Mesuré sur la zone où URTI pose
quelque chose (couche après - avant visible), séparément pour les paires calculée → générée et générée → calculée (DLSS-G).
Aussi : décalage de l'image et de la couche URTI d'une présentation à la suivante (corrélation de phase) : une couche qui ne bouge
qu'une fois sur deux saccade.
Sorties dans le dossier : flip_exces.png (carte de l'excédent moyen), bande.png (huit présentations de suite, zone du reflet).
"""
import os
import re
import sys

import numpy as np
from PIL import Image

import flip_evaluator as flip

M1, M2, C1, C2, C3 = 0.1593017578125, 78.84375, 0.8359375, 18.8515625, 18.6875


def load_raw(p):
    b = open(p, 'rb').read()
    i = b.index(b'\n')
    f, w, h, t = map(int, b[8:i].split())
    v = np.frombuffer(b[i + 1:i + 1 + w * h * 4], dtype=np.uint32).reshape(h, w)
    if f == 24:  # R10G10B10A2, PQ (HDR10, BT.2020)
        e = np.stack([v & 1023, (v >> 10) & 1023, (v >> 20) & 1023], -1).astype(np.float64) / 1023.0
        p_ = np.power(e, 1 / M2)
        nits = 10000 * np.power(np.maximum(p_ - C1, 0) / (C2 - C3 * p_), 1 / M1)
        lin = nits / 203.0
        m = np.array([[1.6605, -0.5876, -0.0728], [-0.1246, 1.1329, -0.0083], [-0.0182, -0.1006, 1.1187]])
        lin = np.maximum(lin @ m.T, 0)
        t_ = lin * (1 + lin / 4) / (1 + lin)  # même épaule que les .ppm des rafales
        return np.clip(t_, 0, 1) ** (1 / 2.2), e.mean(-1)
    c = np.stack([v & 255, (v >> 8) & 255, (v >> 16) & 255], -1).astype(np.float64) / 255.0
    if f == 87:
        c = c[..., ::-1]
    return c, c.mean(-1)


def shift(a, b):
    win = np.outer(np.hanning(a.shape[0]), np.hanning(a.shape[1]))
    A = np.fft.fft2((a - a.mean()) * win)
    B = np.fft.fft2((b - b.mean()) * win)
    R = A.conj() * B
    R /= np.abs(R) + 1e-12
    r = np.real(np.fft.ifft2(R))
    y, x = np.unravel_index(np.argmax(r), r.shape)
    h, w = r.shape

    def sub(v, m, n):
        d = m - 2 * v + n
        return (m - n) / (2 * d) if d != 0 else 0.0
    py = y + sub(r[y, x], r[(y - 1) % h, x], r[(y + 1) % h, x])
    px = x + sub(r[y, x], r[y, (x - 1) % w], r[y, (x + 1) % w])
    return (px - w if px > w / 2 else px), (py - h if py > h / 2 else py)


d = sys.argv[1]
journal = sys.argv[2] if len(sys.argv) > 2 else None
gen = {}
if journal and os.path.exists(journal):
    for line in open(journal, encoding='utf-8', errors='replace'):
        m = re.match(r'FILM (\d+) (\d+) image (\d+) genere (\d) calcul (\d)', line)
        if m:
            gen.setdefault(int(m[1]), {})[int(m[2])] = (int(m[4]), int(m[5]), int(m[3]))
film_no = int(sys.argv[3]) if len(sys.argv) > 3 else (max(gen) if gen else None)
frames = sorted(int(m[1]) for n in os.listdir(d) if (m := re.match(r'avant_(\d+)\.raw$', n)))
A, B, LA, LB = [], [], [], []
for k in frames:
    a, la = load_raw(f'{d}/avant_{k}.raw')
    b, lb = load_raw(f'{d}/apres_{k}.raw')
    A.append(a), B.append(b), LA.append(la), LB.append(lb)
A, B, LA, LB = map(np.array, (A, B, LA, LB))
layer = LB - LA
# zone : là où URTI pose quelque chose de visible dans au moins un tiers des présentations (et pas le HUD, intact)
zone = (np.abs(layer) > 0.01).mean(0) > 0.33
print(f'{len(frames)} présentations, zone URTI {zone.mean() * 100:.1f} % de l\'image')
exA, exB = [], []
sums = np.zeros(A.shape[1:3])
rows = []
for k in range(len(frames) - 1):
    fa, _, _ = flip.evaluate(A[k].astype(np.float32), A[k + 1].astype(np.float32), 'LDR', applyMagma=False, computeMeanError=False)
    fb, _, _ = flip.evaluate(B[k].astype(np.float32), B[k + 1].astype(np.float32), 'LDR', applyMagma=False, computeMeanError=False)
    fa, fb = np.squeeze(fa), np.squeeze(fb)
    ex = np.maximum(fb - fa, 0)
    sums += ex
    sx, sy = shift(LA[k], LA[k + 1])
    lx, ly = shift(layer[k], layer[k + 1])
    g0 = gen.get(film_no, {}).get(frames[k], (-1, -1, 0))
    g1 = gen.get(film_no, {}).get(frames[k + 1], (-1, -1, 0))
    rows.append((k, g0[0], g1[0], fa[zone].mean(), fb[zone].mean(), ex[zone].mean(), sx, sy, lx, ly, np.abs(layer[k + 1] - layer[k])[zone].mean() * 1023))
print(f"{'k':>3} {'gén':>4} {'ꟻLIP jeu':>10} {'ꟻLIP URTI':>11} {'excédent':>9} {'image dx':>9} {'dy':>6} {'couche dx':>10} {'dy':>6} {'|Δcouche|':>10}")
for k, g0, g1, a_, b_, e_, sx, sy, lx, ly, dl in rows:
    print(f'{k:3d} {g0:>2}→{g1:<1} {a_:10.4f} {b_:11.4f} {e_:9.4f} {sx:9.2f} {sy:6.2f} {lx:10.2f} {ly:6.2f} {dl:10.2f}')
r = np.array(rows)
if len(r):
    print(f'moyenne : ꟻLIP jeu {r[:, 3].mean():.4f}, avec URTI {r[:, 4].mean():.4f}, excédent URTI {r[:, 5].mean():.4f} '
          f'({r[:, 5].mean() / max(r[:, 3].mean(), 1e-6) * 100:.0f} % de ce que le jeu change lui-même)')
    for lab, sel in (('calculée → générée', (r[:, 1] == 0) & (r[:, 2] == 1)), ('générée → calculée', (r[:, 1] == 1) & (r[:, 2] == 0))):
        if sel.any():
            print(f'  {lab} : excédent {r[sel, 5].mean():.4f}, |Δcouche| {r[sel, 10].mean():.2f} niveaux, décalage image {np.abs(r[sel, 6]).mean():.2f} px, '
                  f'couche {np.abs(r[sel, 8]).mean():.2f} px')
    m = sums / max(len(rows), 1)
    Image.fromarray((np.clip(m / max(np.percentile(m, 99.5), 1e-6), 0, 1) * 255).astype(np.uint8)).save(os.path.join(d, 'flip_exces.png'))
    ys, xs = np.nonzero(zone)
    if len(ys):
        y0, y1 = max(0, int(np.percentile(ys, 5)) - 20), min(A.shape[1], int(np.percentile(ys, 95)) + 20)
        x0, x1 = max(0, int(np.percentile(xs, 5)) - 20), min(A.shape[2], int(np.percentile(xs, 95)) + 20)
        tiles = [(np.clip(B[k][y0:y1, x0:x1], 0, 1) * 255).astype(np.uint8) for k in range(min(8, len(frames)))]
        h_, w_ = tiles[0].shape[:2]
        sheet = np.zeros((h_ * 2, w_ * 4, 3), np.uint8)
        for i, t in enumerate(tiles):
            sheet[(i // 4) * h_:(i // 4 + 1) * h_, (i % 4) * w_:(i % 4 + 1) * w_] = t
        Image.fromarray(sheet).save(os.path.join(d, 'bande.png'))
