"""Analyse de urti_sandbox_overlap.sh : événements « saut » par pixel et par image.

Un saut à (pixel, t) : la variation de l'image entre t-1 et t s'écarte de plus de 8 % de celle de la
référence au même pixel, au même instant. Références : vérité terrain pour les lancements avec reflets
RT, natif pour les ombres seules (sans reflets RT, la vérité terrain aurait des reflets qui manquent).
On compte les événements (pas des pourcentages qu'on soustrait : les événements se chevauchent) :
  - ombres seules O, reflets seuls R, les deux D ;
  - D expliqué par O ou R, et D qui n'apparaît ni dans O ni dans R : dû à leur superposition ;
  - plancher : natif contre vérité terrain sur les pixels sans reflet RT (ce qui saute déjà sans URTI).
Sortie : superposition.png (rouge : saut seulement quand les deux sont actifs ; vert : expliqué par les
ombres ; bleu : par les reflets), mesures.txt.
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


def series(folder):
    frames = sorted(int(m.group(1)) for m in (re.match(r'urti_photo_(\d+)\.ppm$', n) for n in os.listdir(folder)) if m)
    return frames, [load(os.path.join(folder, f'urti_photo_{f}.ppm')) for f in frames]


def events(run, ref):
    # (images - 1, h, w) : saut à chaque instant
    return np.stack([np.abs((run[i] - run[i - 1]) - (ref[i] - ref[i - 1])).mean(axis=2) > 0.08 for i in range(1, len(run))])


def main():
    out = sys.argv[1]
    names = ['les_deux', 'reflets', 'ombres', 'verite', 'natif']
    data = {}
    for n in names:
        frames, ims = series(os.path.join(out, n))
        data[n] = (frames, ims)
    common = set(data['les_deux'][0])
    for n in names:
        common &= set(data[n][0])
    if len(common) < 3:
        print('pas assez de photos communes')
        return 1
    pick = lambda n: [im for f, im in zip(*data[n]) if f in common]
    D = events(pick('les_deux'), pick('verite'))
    R = events(pick('reflets'), pick('verite'))
    O = events(pick('ombres'), pick('natif'))
    total = D.size
    lines = [f'événements (pixel × image) : {total}']
    pct = lambda x: f'{x.sum() / total * 100:.3f} %'
    lines.append(f'sauts, ombres + reflets : {pct(D)}')
    lines.append(f'sauts, reflets seuls : {pct(R)} ; ombres seules : {pct(O)}')
    only_both = D & ~R & ~O
    lines.append(f'sauts des deux expliqués par les ombres seules : {pct(D & O)} ; par les reflets seuls : {pct(D & R & ~O)}')
    lines.append(f'sauts qui n\'existent que quand les deux sont actifs (superposition) : {pct(only_both)} '
                 f'({only_both.sum() / max(D.sum(), 1) * 100:.1f} % des sauts)')
    lines.append(f'sauts des ombres seules qui disparaissent avec les reflets : {pct(O & ~D)}')
    # plancher : la vérité terrain contre le natif, là où les reflets RT ne touchent rien
    natif_vs_verite = events(pick('natif'), pick('verite'))
    lines.append(f'plancher (natif contre vérité terrain : reflets idéaux qui bougent) : {pct(natif_vs_verite)}')
    text = '\n'.join(lines)
    print(text)
    with open(os.path.join(out, 'mesures.txt'), 'w', encoding='utf-8') as f:
        f.write(text + '\n')
    base = pick('les_deux')[0] * 0.3
    k = max(1.0, len(D) * 0.2)
    base[..., 0] = np.maximum(base[..., 0], np.clip(only_both.sum(0) / k, 0, 1))
    base[..., 1] = np.maximum(base[..., 1], np.clip((D & O).sum(0) / k, 0, 1))
    base[..., 2] = np.maximum(base[..., 2], np.clip((D & R & ~O).sum(0) / k, 0, 1))
    Image.fromarray((base * 255).astype(np.uint8)).save(os.path.join(out, 'superposition.png'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
