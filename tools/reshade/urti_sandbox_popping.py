"""Comptage du « pop / dépop » des reflets RT (photos de la vue de diagnostic 9 : part appliquée).

Bascule à (pixel, t) : la part du reflet RT change de plus de 0,3 d'une image à la suivante.
Aller-retour : bascule dans un sens puis dans l'autre à l'image suivante (le reflet apparaît puis
disparaît aussitôt, ou l'inverse) : c'est le clignotement rapide qu'on voit dans le jeu.
Les comptes sont rapportés aux pixels qui reçoivent un reflet RT (part moyenne > 0,05).
Sortie : bascules.png (rouge : allers-retours cumulés), mesures.txt.
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
    a = np.frombuffer(data[m.end():m.end() + w * h * 3], np.uint8).reshape(h, w, 3).astype(np.float32) / 255.0
    return a.mean(axis=2)


def main():
    folder = sys.argv[1]
    frames = sorted(int(m.group(1)) for m in (re.match(r'urti_photo_(\d+)\.ppm$', n) for n in os.listdir(folder)) if m)
    ims = [load(os.path.join(folder, f'urti_photo_{f}.ppm')) for f in frames]
    if len(ims) < 3:
        print('pas assez de photos')
        return 1
    # les photos sont encodées pour l'écran (sRGB ou PQ) : seuils sur la valeur affichée
    stack = np.stack(ims)
    covered = stack.mean(axis=0) > 0.05
    d = np.diff(stack, axis=0)
    flips = np.abs(d) > 0.3
    back = (d[1:] * d[:-1] < 0) & flips[1:] & flips[:-1]
    n = max(covered.sum(), 1)
    lines = [f'pixels avec reflet RT : {covered.mean() * 100:.1f} % de l\'image',
             f'bascules (part appliquée qui change de plus de 0,3) : {flips[:, covered].mean() * 100:.3f} % des pixels reflétants par image',
             f'allers-retours (apparaît puis disparaît aussitôt) : {back[:, covered].mean() * 100:.3f} % des pixels reflétants par image']
    text = '\n'.join(lines)
    print(text)
    with open(os.path.join(folder, 'mesures.txt'), 'w', encoding='utf-8') as f:
        f.write(text + '\n')
    heat = np.clip(back.sum(axis=0) / max(1.0, len(back) * 0.2), 0, 1)
    img = np.stack([np.maximum(stack[0] * 0.4, heat), stack[0] * 0.4, stack[0] * 0.4], axis=2)
    Image.fromarray((img[::2, ::2] * 255).astype(np.uint8)).save(os.path.join(folder, 'bascules.png'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
