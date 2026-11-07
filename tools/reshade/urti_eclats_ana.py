"""URTI : points brillants ajoutés par URTI, sur chaque rafale (urti_rafale_N, avant / après) d'un dossier.

    python urti_eclats_ana.py <dossier contenant les rafales>

Colonnes : part des pixels éclaircis de plus de 5, 10, 20 %, assombris de plus de 10 % ; points isolés plus clairs que leur voisinage ajoutés par URTI (nombre,
pixels), ceux déjà dans l'image du jeu ; variation de l'effet d'une image à la suivante. Pour trouver quel effet les fabrique, couper reflect=, gi=, ao= dans
urti.ini entre deux rafales (relu à chaud). Sortie dans chaque rafale : ecl/rouge_ajout.png (en rouge ce qu'URTI éclaircit).
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
print(f"{'rafale':24} {'+>.05':>7} {'+>.1':>7} {'+>.2':>7} {'->.1':>7} | {'points':>6} {'pix':>6} | {'jeu pts':>7} | {'|dC|>.05':>8} {'|dC|>.1':>8}")
for name in sorted(os.listdir(root)):
    f = os.path.join(root, name)
    if not os.path.isdir(f) or not os.path.exists(f + '/apres_0.ppm'):
        continue
    idx = sorted(int(m[1]) for n in os.listdir(f) if (m := re.match(r'apres_(\d+)\.ppm$', n)))
    A = np.stack([load(f'{f}/avant_{i}.ppm').mean(2) for i in idx])
    B = np.stack([load(f'{f}/apres_{i}.ppm').mean(2) for i in idx])
    C = B - A
    pos = np.clip(C, 0, 1)
    neg = np.clip(-C, 0, 1)
    pts = []
    game = []
    # un image sur trois : points brillants plus clairs que leur voisinage, ajoutés par URTI / déjà dans l'image du jeu
    for k in range(0, len(idx), 3):
        s = (B[k] - median_filter(B[k], size=9) > 0.12) & (pos[k] > 0.08)
        lab, n = label(s)
        pts.append((n, int(s.sum())))
        g = A[k] - median_filter(A[k], size=9) > 0.12
        game.append(label(g)[1])
    dC = np.abs(C[1:] - C[:-1])
    print(f"{name:24} {100*(pos>.05).mean():7.3f} {100*(pos>.1).mean():7.3f} {100*(pos>.2).mean():7.3f} {100*(neg>.1).mean():7.3f} | "
          f"{np.mean([p[0] for p in pts]):6.1f} {np.mean([p[1] for p in pts]):6.1f} | {np.mean(game):7.1f} | {100*(dC>.05).mean():8.3f} {100*(dC>.1).mean():8.3f}")
    out = f + '/ecl'
    os.makedirs(out, exist_ok=True)
    g8 = lambda x: (np.clip(x, 0, 1) * 255).astype(np.uint8)
    k = len(idx) // 2
    Image.fromarray(np.stack([g8(B[k] * 0.6 + pos[k] * 4), g8(B[k] * 0.6), g8(B[k] * 0.6)], 2)).save(out + '/rouge_ajout.png')
    Image.fromarray(g8(B[k])).save(out + '/apres.png')
    Image.fromarray(g8(A[k])).save(out + '/avant.png')
