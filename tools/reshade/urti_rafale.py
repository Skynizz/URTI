"""URTI : analyse d'une rafale prise dans le jeu (touche F8, ou fichier déclencheur urti_rafale
dans le dossier du jeu).

    python urti_rafale.py <dossier urti_rafale_N> <dossier de sortie> [journal urti_log.txt]

La rafale contient, pour 30 images consécutives, l'image du jeu sans URTI (avant_N.ppm) et la même image avec
URTI (apres_N.ppm). La contribution d'URTI est C = après - avant ; ce qui saute est la variation de C d'une
image à la suivante, indépendante de ce que fait la scène (vent, personnage, caméra lente). Mesures :
  - effet : part des pixels qu'URTI change de plus de 2 % (ombres : il assombrit ; reflets : le reste) ;
  - tremble / saute : part des pixels dont C varie de plus de 3 % / 8 % d'une image à la suivante, en tout
    et dans chaque zone (ombres, reflets) ;
  - mouvement de la scène : variation moyenne de l'image sans URTI (caméra immobile : petite) ;
  - par image : pixels qui sautent, objets entrés / sortis de la scène RT (lignes RAFALE du journal).
Sortie : carte.png (image avec URTI assombrie, en rouge les pixels qui sautent, plus c'est rouge plus souvent),
zone.png (la zone de 96 × 96 pixels qui saute le plus, sur six images consécutives : sans URTI en haut, avec
en bas), mesures.txt.
"""
import os
import re
import sys

import numpy as np
from PIL import Image


def load(path):
    with open(path, 'rb') as f:
        data = f.read()
    m = re.match(rb'P6\s+(\d+)\s+(\d+)\s+(\d+)\s', data)
    w, h = int(m.group(1)), int(m.group(2))
    return np.frombuffer(data[m.end():m.end() + w * h * 3], np.uint8).reshape(h, w, 3).astype(np.float32) / 255.0


def main():
    folder, out = sys.argv[1], sys.argv[2]
    log = sys.argv[3] if len(sys.argv) > 3 else None
    os.makedirs(out, exist_ok=True)
    idx = sorted(int(m.group(1)) for n in os.listdir(folder) if (m := re.match(r'apres_(\d+)\.ppm$', n)))
    idx = [i for i in idx if os.path.exists(os.path.join(folder, f'avant_{i}.ppm'))]
    if len(idx) < 2:
        sys.exit('rafale incomplète')
    before = [load(os.path.join(folder, f'avant_{i}.ppm')) for i in idx]
    after = [load(os.path.join(folder, f'apres_{i}.ppm')) for i in idx]
    contrib = [a - b for a, b in zip(after, before)]
    mean_c = np.mean(contrib, axis=0).mean(axis=2)
    strength = np.mean([np.abs(c).max(axis=2) for c in contrib], axis=0)
    shadow_zone = (mean_c < -0.01) & (strength > 0.02)
    refl_zone = (strength > 0.02) & ~shadow_zone
    h, w = mean_c.shape
    jumps = np.zeros((h, w), np.float32)
    lines = []
    rows = []
    trem_all = jump_all = 0.0
    trem_s = jump_s = trem_r = jump_r = 0.0
    scene = 0.0
    for k in range(1, len(idx)):
        d = np.abs(contrib[k] - contrib[k - 1]).max(axis=2)
        scene += float(np.abs(before[k] - before[k - 1]).mean())
        t, j = d > 0.03, d > 0.08
        jumps += j
        trem_all += t.mean()
        jump_all += j.mean()
        if shadow_zone.any():
            trem_s += t[shadow_zone].mean()
            jump_s += j[shadow_zone].mean()
        if refl_zone.any():
            trem_r += t[refl_zone].mean()
            jump_r += j[refl_zone].mean()
        rows.append((idx[k], 100.0 * j.mean(), 100.0 * (j[refl_zone].mean() if refl_zone.any() else 0.0),
                     100.0 * (j[shadow_zone].mean() if shadow_zone.any() else 0.0)))
    n = len(idx) - 1
    churn = {}
    burst = re.search(r'(\d+)$', os.path.normpath(folder))
    if log and os.path.exists(log) and burst:
        with open(log, encoding='utf-8', errors='replace') as f:
            for line in f:
                m = re.match(r'RAFALE (\d+) (\d+) image \d+ objets RT (\d+) entrés (\d+) sortis (\d+)', line)
                if m and m.group(1) == burst.group(1):
                    churn[int(m.group(2))] = (int(m.group(3)), int(m.group(4)), int(m.group(5)))
    lines.append(f'images : {len(idx)} ({w}x{h})')
    lines.append(f'mouvement de la scène (sans URTI) : {100.0 * scene / n:.2f} % par image')
    lines.append(f'effet d\'URTI : {100.0 * (strength > 0.02).mean():.1f} % des pixels changés de plus de 2 % '
                 f'(ombres {100.0 * shadow_zone.mean():.1f} %, reflets {100.0 * refl_zone.mean():.1f} %)')
    lines.append(f'tremble (> 3 %) : {100.0 * trem_all / n:.2f} % des pixels ; ombres {100.0 * trem_s / n:.2f} %, reflets {100.0 * trem_r / n:.2f} % (de leur zone)')
    lines.append(f'saute (> 8 %) : {100.0 * jump_all / n:.3f} % des pixels ; ombres {100.0 * jump_s / n:.2f} %, reflets {100.0 * jump_r / n:.2f} % (de leur zone)')
    lines.append('par image : n°, saute % (tout, reflets, ombres), objets RT, entrés, sortis')
    for i, a, r, s in rows:
        c = churn.get(i)
        lines.append(f'  {i:3d}  {a:6.3f} {r:6.2f} {s:6.2f}' + (f'   {c[0]:4d} {c[1]:4d} {c[2]:4d}' if c else ''))
    text = '\n'.join(lines)
    print(text)
    with open(os.path.join(out, 'mesures.txt'), 'w', encoding='utf-8') as f:
        f.write(text + '\n')
    # carte : image avec URTI assombrie, sauts en rouge
    base = after[-1] * 0.45
    heat = np.clip(jumps / max(1.0, n * 0.25), 0, 1)
    base[..., 0] = np.maximum(base[..., 0], heat)
    Image.fromarray((np.clip(base, 0, 1) * 255).astype(np.uint8)).save(os.path.join(out, 'carte.png'))
    # zone qui saute le plus : six images consécutives, sans URTI en haut, avec en bas
    s = 96
    if h > s and w > s:
        acc = jumps.cumsum(0).cumsum(1)
        best, by, bx = -1.0, 0, 0
        for y in range(0, h - s, 16):
            for x in range(0, w - s, 16):
                v = acc[y + s, x + s] - acc[y, x + s] - acc[y + s, x] + acc[y, x]
                if v > best:
                    best, by, bx = v, y, x
        k0 = max(0, int(np.argmax([r[1] for r in rows])) - 2)
        frames = list(range(k0, min(len(idx), k0 + 6)))
        strip = np.zeros((2 * s, len(frames) * s, 3), np.float32)
        for c, k in enumerate(frames):
            strip[:s, c * s:(c + 1) * s] = before[k][by:by + s, bx:bx + s]
            strip[s:, c * s:(c + 1) * s] = after[k][by:by + s, bx:bx + s]
        Image.fromarray((strip * 255).astype(np.uint8)).resize((len(frames) * s * 3, 2 * s * 3), Image.NEAREST).save(os.path.join(out, 'zone.png'))
        with open(os.path.join(out, 'mesures.txt'), 'a', encoding='utf-8') as f:
            f.write(f'zone qui saute le plus : x {bx}, y {by} (images {idx[frames[0]]} à {idx[frames[-1]]})\n')


if __name__ == '__main__':
    main()
