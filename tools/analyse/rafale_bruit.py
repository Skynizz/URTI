"""Bruit temporel qu'URTI ajoute dans une rafale (avant_k / apres_k .ppm) : écart type d'une image à l'autre, avec URTI moins sans, par pixel.
rafale_bruit.py <dossier> [<dossier> ...]  ->  carte <dossier>/bruit.png ; chiffres : moyenne, part des pixels à plus de 3 niveaux, 99e centile."""
import glob
import os
import sys

import numpy as np
from PIL import Image


def stack(d, which):
    fs = sorted(glob.glob(os.path.join(d, which + '_*.ppm')), key=lambda s: int(s.rsplit('_', 1)[1].split('.')[0]))
    return np.stack([np.asarray(Image.open(f)).astype(np.float32).mean(-1) for f in fs])


for d in sys.argv[1:]:
    a, b = stack(d, 'avant'), stack(d, 'apres')
    # écart d'une image à la suivante (pas l'écart type global : une lente dérive de la scène n'est pas du bruit)
    da = np.abs(np.diff(a, axis=0)).mean(0)
    db = np.abs(np.diff(b, axis=0)).mean(0)
    ex = db - da
    Image.fromarray((np.clip(ex / 4, 0, 1) * 255).astype(np.uint8)).save(os.path.join(d, 'bruit.png'))
    print('%-28s images %d | variation jeu %.3f, avec URTI %.3f | excédent moyen %.3f, pixels > 3 niveaux %.2f %%, 99e centile %.2f' % (
        os.path.basename(d.rstrip('/\\')), len(a), da.mean(), db.mean(), ex.mean(), (ex > 3).mean() * 100, np.percentile(ex, 99)))
