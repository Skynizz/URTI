"""Biais de l'indirect non filtré de KRC face à la référence (mode 3), réglage par réglage.

Chaque variante est une session figée : KRC (affichage de l'indirect non filtré), puis la
référence accumulée dans la même session. Le rapport KRC / référence est donné par bande de
luminance de la référence (une perte d'énergie dans les zones sombres se voit tout de suite).

  python -I tools/doomrt/indirect_bias.py --variant base= --variant sans_rs=KRC_RESAMPLE=0
"""

import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench  # noqa: E402
import ppm  # noqa: E402

BANDS = ((0, 0.02), (0.02, 0.05), (0.05, 0.1), (0.1, 0.3), (0.3, 2.0))


def blur(a, r=6):
    k = np.ones(2 * r + 1) / (2 * r + 1)
    b = np.apply_along_axis(lambda x: np.convolve(x, k, 'same'), 0, a)
    return np.apply_along_axis(lambda x: np.convolve(x, k, 'same'), 1, b)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--variant', action='append', default=[], help='nom=NOM1=v1,NOM2=v2')
    ap.add_argument('--map', type=int, default=1)
    ap.add_argument('--acc', type=int, default=512)
    ap.add_argument('--kacc', type=int, default=64, help='frames moyennées pour KRC')
    a = ap.parse_args()

    k_end = 600 + a.kacc
    script = ['100:pause', '101:expo', '102:show=32', '110:mode=2', f'{k_end + 10}:mode=3']
    captures = [f'600+{a.kacc}', f'{k_end + 310}+{a.acc}']
    for v in a.variant:
        name, _, settings = v.partition('=')
        env = dict(s.split('=', 1) for s in settings.split(',') if s)
        out = os.path.join(bench.GAME, 'cap', 'biais', name)
        bench.run_session(a.map, out, script, captures, env, 1200)
        k = ppm.read(os.path.join(out, 'f00600.ppm'))
        r = ppm.read(os.path.join(out, f'f{k_end + 310:05d}.ppm'))
        h, w, _ = r.shape
        mask = np.ones((h, w), bool)
        mask[int(h * 0.62):, int(w * 0.36):int(w * 0.66)] = False  # l'arme
        lk, lr = blur(ppm.luminance(k)), blur(ppm.luminance(r))
        bands = []
        for lo, hi in BANDS:
            m = mask & (lr >= lo) & (lr < hi)
            bands.append(f'{lk[m].mean() / lr[m].mean():.3f}' if m.sum() else '-')
        print(f'{name:12s} lum {ppm.luminance(k)[mask].mean() / ppm.luminance(r)[mask].mean():.3f} '
              f'| par bande {" ".join(bands)} | PSNR {ppm.psnr(k, r, mask):.2f}', flush=True)


if __name__ == '__main__':
    main()
