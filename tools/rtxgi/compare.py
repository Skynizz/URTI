"""Comparaison robuste des méthodes dans le path tracer d'exemple de NVIDIA (branche krc).

Une session invisible par scène, tout à la même image (jitter coupé, comme NRD) :
  - référence : chemins complets accumulés ;
  - pour chaque méthode : 8 frames débruitées par NRD (ꟻLIP moyen : ce que voit le joueur),
    8 frames brutes (bruit avant NRD), une moyenne de frames (biais du cache seul), temps GPU.

  python -I tools/rtxgi/compare.py --scene LivingRoom.scene.json --name salon --methods sharc sharc_guide
"""

import argparse
import json
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import img  # noqa: E402
import run  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

METHODS = {
    'sharc': ['tech=sharc', 'scale=50', 'guide=0'],
    'sharc_guide': ['tech=sharc', 'scale=50', 'guide=1'],
    'krca': ['tech=krc', 'scale=50', 'guide=0'],
    'krca_guide': ['tech=krc', 'scale=50', 'guide=1'],
    'path': ['tech=ref', 'guide=0'],   # path tracing complet sans cache, même débruiteur
}
FRAMES = 8


def plan(methods, ref_spp, warm, acc):
    cmds = ['20:jitter=0', '20:tech=ref', '20:den=acc', f'{20 + ref_spp}:capture=ref']
    f = 25 + ref_spp
    for name in methods:
        cmds += [f'{f}:{c}' for c in METHODS[name] + ['den=nrd']]
        cmds.append(f'{f + warm - 110}:fps=100')
        cmds += [f'{f + warm + k}:capture={name}_nrd{k}' for k in range(FRAMES)]
        g = f + warm + FRAMES
        cmds.append(f'{g}:den=none')
        cmds += [f'{g + 10 + k}:capture={name}_raw{k}' for k in range(FRAMES)]
        g += 10 + FRAMES
        cmds.append(f'{g}:den=acc')
        cmds.append(f'{g + acc}:capture={name}_acc')
        f = g + acc + 5
    cmds.append(f'{f}:exit')
    return ';'.join(cmds)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--scene', default='bistro.scene.json')
    ap.add_argument('--name', default='bistro')
    ap.add_argument('--methods', nargs='+', default=['sharc', 'sharc_guide'])
    ap.add_argument('--ref', type=int, default=2048)
    ap.add_argument('--warm', type=int, default=250)
    ap.add_argument('--acc', type=int, default=512)
    ap.add_argument('--eval-only', action='store_true')
    a = ap.parse_args()

    out = f'F:/rtxgi/out/cmp_{a.name}'
    if not a.eval_only:
        if os.path.exists(f'{out}/fps.txt'):
            os.remove(f'{out}/fps.txt')
        print(run.run(out, plan(a.methods, a.ref, a.warm, a.acc), ['-scene', a.scene]))

    ref = img.read_pfm(f'{out}/ref.pfm')
    dst = os.path.join(ROOT, 'results', 'rtxgi', a.name)
    os.makedirs(dst, exist_ok=True)
    img.write_png(os.path.join(dst, 'reference.png'), img.tonemap(ref))
    lines = [l for l in open(f'{out}/fps.txt').read().splitlines() if l.strip()] if os.path.exists(f'{out}/fps.txt') else []
    gpu = {n: float(l.split('gpu_ms')[1].split()[0]) for n, l in zip(a.methods, lines)}

    res = {}
    for name in a.methods:
        nrd = [img.read_pfm(f'{out}/{name}_nrd{k}.pfm') for k in range(FRAMES)]
        raw = np.stack([img.tonemap(img.read_pfm(f'{out}/{name}_raw{k}.pfm')) for k in range(FRAMES)])
        acc = img.read_pfm(f'{out}/{name}_acc.pfm')
        flips = [img.flip(x, ref)[0] for x in nrd]
        r = {
            'nrd_flip': float(np.mean(flips)), 'nrd_flip_ecart': float(np.std(flips)),
            'nrd_psnr': float(np.mean([img.psnr(x, ref) for x in nrd])),
            'bruit_brut': float(raw.std(0).mean()),
            'biais_flip': img.flip(acc, ref)[0], 'biais_relmse': img.rel_mse(acc, ref),
            'lum': float(acc.mean() / ref.mean()), 'gpu_ms': gpu.get(name, 0.0),
        }
        res[name] = r
        img.write_png(os.path.join(dst, f'{name}_nrd.png'), img.tonemap(nrd[0]))
        img.write_png(os.path.join(dst, f'{name}_brut.png'), raw[0])
        img.write_png(os.path.join(dst, f'{name}_flip.png'), img.flip(nrd[0], ref)[1])
        print(f'{name:12s} NRD FLIP {r["nrd_flip"]:.4f} +- {r["nrd_flip_ecart"]:.4f} PSNR {r["nrd_psnr"]:.2f} | '
              f'bruit brut {r["bruit_brut"]:.4f} | biais FLIP {r["biais_flip"]:.4f} relMSE {r["biais_relmse"]:.4f} '
              f'lum {r["lum"]:.3f} | GPU {r["gpu_ms"]:.2f} ms', flush=True)
    with open(os.path.join(dst, 'comparaison.json'), 'w') as f:
        json.dump({'reglages': vars(a), 'resultats': res}, f, indent=1)


if __name__ == '__main__':
    main()
