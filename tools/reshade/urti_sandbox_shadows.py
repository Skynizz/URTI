"""Bac à sable URTI : les ombres ajoutées par URTI ressemblent-elles à celles du jeu ?

    python urti_sandbox_shadows.py <dossier RT> <dossier complet> <dossier partiel> <dossier de sortie>

Trois lancements identiques (mêmes images) :
  - RT : la carte d'ombres du « jeu » oublie des objets (URTI_SANDBOX_GAMESHADOW=partiel), URTI les ombre ;
  - complet : natif, le jeu ombre tout : la référence, ce qu'URTI devrait donner ;
  - partiel : natif avec les ombres oubliées : ce qu'on aurait sans URTI.
Zone mesurée : les pixels que l'ombre complète assombrit et que la partielle laisse éclairés (les ombres
qu'URTI doit ajouter). Mesures :
  - rattrapé : part de l'écart partiel / complet qu'URTI comble (100 % : ombres identiques à celles du jeu) ;
  - noirceur : lumière moyenne de la zone avec URTI / avec le jeu, par canal (1 : pareil ; > 1 : trop
    claire ; < 1 : trop sombre) ; un écart entre canaux est une mauvaise teinte ;
  - ailleurs : écart moyen RT / complet hors de la zone (ombres RT en trop, bords) ;
  - scintillement : variation d'une image à l'autre qui n'est pas dans la référence, dans la zone.
Sortie : ombres.png (complet | partiel | RT | écart amplifié) pour la première image, mesures.txt.
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


def frames(folder):
    out = {}
    for name in os.listdir(folder):
        m = re.match(r'urti_photo_(\d+)\.ppm$', name)
        if m:
            out[int(m.group(1))] = os.path.join(folder, name)
    return out


def main():
    rt_dir, full_dir, part_dir, out_dir = sys.argv[1:5]
    os.makedirs(out_dir, exist_ok=True)
    rt, full, part = frames(rt_dir), frames(full_dir), frames(part_dir)
    keys = sorted(set(rt) & set(full) & set(part))
    if not keys:
        sys.exit('aucune image commune')
    lum = np.array([0.2126, 0.7152, 0.0722], np.float32)
    gap_rt = gap_none = 0.0
    zone_pixels = 0
    sum_rt = np.zeros(3)
    sum_full = np.zeros(3)
    sum_part = np.zeros(3)
    elsewhere = elsewhere_n = 0.0
    flicker = flicker_n = 0.0
    prev = None
    for i, k in enumerate(keys):
        a, b, c = load(rt[k]), load(full[k]), load(part[k])
        # lumière (photo en gamma 2,2)
        la, lb, lc = a ** 2.2, b ** 2.2, c ** 2.2
        yb, yc = lb @ lum, lc @ lum
        zone = (yc - yb > 0.02) & (yc > 0.05)
        n = int(zone.sum())
        if n:
            gap_rt += float(np.abs(la - lb)[zone].sum())
            gap_none += float(np.abs(lc - lb)[zone].sum())
            sum_rt += la[zone].sum(axis=0)
            sum_full += lb[zone].sum(axis=0)
            sum_part += lc[zone].sum(axis=0)
            zone_pixels += n
        other = ~zone
        elsewhere += float(np.abs(a - b)[other].mean())
        elsewhere_n += 1
        if prev is not None:
            pa, pb, pzone = prev
            both = zone & pzone
            if both.any():
                flicker += float(np.abs((a - pa) - (b - pb))[both].mean())
                flicker_n += 1
        prev = (a, b, zone)
        if i == 0:
            diff = np.clip(np.abs(a - b) * 4.0, 0, 1)
            row = np.concatenate([b, c, a, diff], axis=1)
            Image.fromarray((row * 255).astype(np.uint8)).save(os.path.join(out_dir, 'ombres.png'))
    lines = [f'images comparées : {len(keys)} ({keys[0]} à {keys[-1]})']
    if zone_pixels:
        mean_full, mean_rt, mean_part = sum_full / zone_pixels, sum_rt / zone_pixels, sum_part / zone_pixels
        ratio = mean_rt / np.maximum(mean_full, 1e-6)
        lines.append(f'zone des ombres à ajouter : {zone_pixels / len(keys):.0f} pixels par image')
        lines.append(f'rattrapé (écart sans URTI comblé) : {100.0 * (1.0 - gap_rt / max(gap_none, 1e-9)):.1f} %')
        lines.append('noirceur (lumière RT / jeu, rouge vert bleu) : ' + ' '.join(f'{v:.3f}' for v in ratio))
        lines.append('sans URTI (partiel / jeu) : ' + ' '.join(f'{v:.3f}' for v in mean_part / np.maximum(mean_full, 1e-6)))
    else:
        lines.append('zone des ombres à ajouter : vide')
    lines.append(f'ailleurs (écart moyen RT / jeu hors de la zone) : {100.0 * elsewhere / max(elsewhere_n, 1):.3f} %')
    lines.append(f'scintillement ajouté dans la zone : {100.0 * flicker / max(flicker_n, 1):.3f} %')
    text = '\n'.join(lines)
    print(text)
    with open(os.path.join(out_dir, 'mesures.txt'), 'w', encoding='utf-8') as f:
        f.write(text + '\n')


if __name__ == '__main__':
    main()
