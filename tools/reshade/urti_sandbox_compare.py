"""Bac à sable URTI : compare un lancement avec RT et un lancement natif, image par image.

    python urti_sandbox_compare.py <dossier RT> <dossier natif> <dossier de sortie>

Les deux dossiers contiennent urti_photo_<présentation>.ppm aux mêmes présentations (le bac à sable est
déterministe : même caméra, même vent, mêmes passants). Mesures :
  - effet : écart moyen entre RT et natif (ce que le RT change à l'image) ;
  - scintillement : écart moyen d'une image à la suivante ; le natif donne celui de la scène elle-même
    (vent, passants, caméra) ; « ajouté » = RT - natif, ce qui saute à cause d'URTI ;
  - points qui sautent : part des pixels dont la variation d'une image à la suivante s'écarte de plus de
    8 % de celle de la référence au même endroit (les ombres ou reflets qui clignotent). La référence est
    le natif, ou la vérité terrain (urti_sandbox.sh --verite) : l'écart moyen est alors l'erreur des
    reflets RT, et un reflet qui bouge normalement ne compte pas comme un saut.
Sortie : comparaison.png (natif | RT | écart amplifié) pour la première image, scintillement.png (carte
des sauts ajoutés, cumulée sur la série), mesures.txt.
"""
import os
import re
import sys

import numpy as np
from PIL import Image


def load(path):
    with open(path, 'rb') as f:
        data = f.read()
    # en-tête P6 : largeur hauteur maximum
    m = re.match(rb'P6\s+(\d+)\s+(\d+)\s+(\d+)\s', data)
    w, h = int(m.group(1)), int(m.group(2))
    return np.frombuffer(data[m.end():m.end() + w * h * 3], np.uint8).reshape(h, w, 3).astype(np.float32) / 255.0


def frames(folder):
    out = {}
    for name in os.listdir(folder):
        m = re.match(r'urti_photo_(\d+)\.ppm$', name)
        if m:
            out[int(m.group(1))] = os.path.join(folder, name)
    return out


def main():
    rt_dir, native_dir, out_dir = sys.argv[1:4]
    os.makedirs(out_dir, exist_ok=True)
    rt, native = frames(rt_dir), frames(native_dir)
    common = sorted(set(rt) & set(native))
    if not common:
        print('aucune photo commune')
        return 1
    lines = []
    effect = []
    prev_rt = prev_nat = None
    flick_rt, flick_nat, jumps, blinks = [], [], [], []
    prev_d = None
    jump_map = None
    for i, f in enumerate(common):
        a, b = load(rt[f]), load(native[f])
        effect.append(float(np.abs(a - b).mean()))
        if i == 0:
            diff = np.clip(np.abs(a - b) * 4, 0, 1)
            Image.fromarray((np.concatenate([b, a, diff], axis=1) * 255).astype(np.uint8)).save(os.path.join(out_dir, 'comparaison.png'))
            Image.fromarray((a * 255).astype(np.uint8)).save(os.path.join(out_dir, f'rt_{f}.png'))
            Image.fromarray((b * 255).astype(np.uint8)).save(os.path.join(out_dir, f'natif_{f}.png'))
        if prev_rt is not None and f == common[i - 1] + 1:
            d_rt = np.abs(a - prev_rt).mean(axis=2)
            d_nat = np.abs(b - prev_nat).mean(axis=2)
            flick_rt.append(float(d_rt.mean()))
            flick_nat.append(float(d_nat.mean()))
            # variation propre au RT : ce qui change d'une image à la suivante autrement que la référence
            extra = np.abs((a - prev_rt) - (b - prev_nat)).mean(axis=2) > 0.08
            jumps.append(float(extra.mean()))
            jump_map = extra.astype(np.float32) if jump_map is None else jump_map + extra
            # clignotement : l'écart propre au RT change de sens d'une image à la suivante (plus clair puis plus
            # sombre, ou l'inverse) ; une erreur qui se déplace avec la scène garde le même sens
            d = ((a - prev_rt) - (b - prev_nat)).mean(axis=2)
            if prev_d is not None:
                blinks.append(float(((d * prev_d < 0) & (np.abs(d) > 0.04) & (np.abs(prev_d) > 0.04)).mean()))
            prev_d = d
        else:
            prev_d = None
        prev_rt, prev_nat = a, b
    lines.append(f'photos comparées : {len(common)} ({common[0]} à {common[-1]})')
    lines.append(f'effet du RT (écart moyen RT / référence) : {np.mean(effect) * 100:.2f} %')
    if flick_rt:
        fr, fn = np.mean(flick_rt), np.mean(flick_nat)
        lines.append(f'scintillement d\'une image à la suivante : RT {fr * 100:.3f} %, natif {fn * 100:.3f} %, ajouté {(fr - fn) * 100:+.3f} %')
        lines.append(f'points qui sautent (variation écartée de la référence de plus de 8 %) : {np.mean(jumps) * 100:.3f} % des pixels, au pire {np.max(jumps) * 100:.3f} %')
        if blinks:
            lines.append(f'points qui clignotent (écart qui change de sens, plus de 4 %) : {np.mean(blinks) * 100:.3f} % des pixels')
        heat = np.clip(jump_map / max(1.0, len(jumps) * 0.25), 0, 1)
        base = load(rt[common[0]]) * 0.35
        base[..., 0] = np.maximum(base[..., 0], heat)
        Image.fromarray((base * 255).astype(np.uint8)).save(os.path.join(out_dir, 'scintillement.png'))
    text = '\n'.join(lines)
    print(text)
    with open(os.path.join(out_dir, 'mesures.txt'), 'w', encoding='utf-8') as f:
        f.write(text + '\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
