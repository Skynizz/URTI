"""Bruit et biais de chaque méthode dans Doom RT, sur une session figée (touche Pause).

Pour chaque méthode : préchauffage, puis N frames consécutives capturées une à une. On en tire
  - le bruit temporel : écart-type d'une frame à l'autre, par pixel (fin : grain ; gros : taches
    qui scintillent, mesuré sur les images floutées) ;
  - le biais : la moyenne des N frames contre la référence (chemins complets, accumulés).
Hors de l'arme du joueur, en luminance d'affichage.

  python -I tools/doomrt/noise.py --maps 1 3 --env KRC_PPP=0.05 KRC_EMIT_PHYS=1
"""

import argparse
import json
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench  # noqa: E402
import ppm  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def blur(a, r=6):
    k = np.ones(2 * r + 1) / (2 * r + 1)
    b = np.apply_along_axis(lambda x: np.convolve(x, k, 'same'), 0, a)
    return np.apply_along_axis(lambda x: np.convolve(x, k, 'same'), 1, b)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--maps', type=int, nargs='+', default=[1])
    ap.add_argument('--methods', type=int, nargs='+', default=[0, 1, 2])
    ap.add_argument('--frames', type=int, default=32)
    ap.add_argument('--warm', type=int, default=400)
    ap.add_argument('--acc', type=int, default=256)
    ap.add_argument('--name', default='bruit')
    ap.add_argument('--env', nargs='*', default=[])
    a = ap.parse_args()

    script = ['100:pause', '101:expo']
    captures, frames_of = [], {}
    f = 110
    for m in a.methods:
        script.append(f'{f}:mode={m}')
        first = f + a.warm
        frames_of[m] = list(range(first, first + a.frames))
        captures += frames_of[m]
        f = first + a.frames + 10
    script.append(f'{f}:mode=3')
    ref_frame = f + 300
    captures.append(f'{ref_frame}+{a.acc}')
    env = dict(e.split('=', 1) for e in a.env)

    summary = {'reglages': vars(a), 'cartes': {}}
    for mp in a.maps:
        out = os.path.join(bench.GAME, 'cap', a.name, f'E1M{mp}')
        print(f'E1M{mp} ...', flush=True)
        bench.run_session(mp, out, script, captures, env, 1500)
        ref = ppm.read(os.path.join(out, f'f{ref_frame:05d}.ppm'))
        mask = bench.weapon_mask(ref.shape)
        lref = ppm.luminance(ref)
        res = {}
        for m in a.methods:
            lum = np.stack([ppm.luminance(ppm.read(os.path.join(out, f'f{k:05d}.ppm'))) for k in frames_of[m]])
            mean = lum.mean(0)
            fine = lum.std(0)
            coarse = np.stack([blur(x) for x in lum]).std(0)
            scale = lref[mask].mean()
            r = {
                'bruit_fin': float(fine[mask].mean() / scale),       # relatif à la luminance moyenne
                'bruit_taches': float(coarse[mask].mean() / scale),
                'biais_psnr': float(10 * np.log10(1.0 / max(((mean - lref) ** 2)[mask].mean(), 1e-12))),
                'psnr_frame': float(np.mean([10 * np.log10(1.0 / max(((x - lref) ** 2)[mask].mean(), 1e-12))
                                             for x in lum])),
            }
            res[bench.NAMES[m]] = r
            print(f'  {bench.NAMES[m]:6s} bruit fin {r["bruit_fin"]:.4f}  taches {r["bruit_taches"]:.4f}  '
                  f'| PSNR moyenne (biais) {r["biais_psnr"]:.2f}  PSNR par frame {r["psnr_frame"]:.2f}', flush=True)
        summary['cartes'][f'E1M{mp}'] = res
    dst = os.path.join(ROOT, 'results', 'doomrt', a.name)
    os.makedirs(dst, exist_ok=True)
    with open(os.path.join(dst, 'mesures.json'), 'w') as fh:
        json.dump(summary, fh, indent=1)


if __name__ == '__main__':
    main()
