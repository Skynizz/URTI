"""KRC-A (cellules adaptatives) contre SHaRC dans le path tracer d'exemple de NVIDIA.

Une session invisible par scène : référence (chemins complets accumulés), puis pour chaque méthode
une frame débruitée par NRD (ce que voit le joueur), une moyenne de frames (biais seul du cache) et
le temps GPU. Tout à la même image (jitter coupé, comme NRD).

  python -I tools/rtxgi/krca.py --scene LivingRoom.scene.json --name salon
Sorties : F:/rtxgi/out/<nom>/*.pfm, résultats/rtxgi/<nom>/ (PNG, mesures.json)
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

# nom : (commandes de la méthode)
METHODS = {
    'sharc50': ['tech=sharc', 'scale=50'],
    'sharc100': ['tech=sharc', 'scale=100'],
    'krca100': ['tech=krc', 'scale=100'],
}


def plan(methods, ref_spp, warm, acc):
    cmds = ['20:jitter=0', '20:tech=ref', '20:den=acc']
    f = 20 + ref_spp
    cmds.append(f'{f}:capture=ref')
    f += 5
    for name in methods:
        for c in METHODS[name] + ['den=nrd']:
            cmds.append(f'{f}:{c}')
        cmds.append(f'{f + warm - 110}:fps=100')
        cmds.append(f'{f + warm}:capture={name}_nrd')
        cmds.append(f'{f + warm + 1}:den=acc')
        cmds.append(f'{f + warm + 1 + acc}:capture={name}_acc')
        f += warm + acc + 5
    cmds.append(f'{f}:exit')
    return ';'.join(cmds)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--scene', default='bistro.scene.json')
    ap.add_argument('--name', default='bistro')
    ap.add_argument('--methods', nargs='+', default=list(METHODS))
    ap.add_argument('--ref', type=int, default=2048)
    ap.add_argument('--warm', type=int, default=250)
    ap.add_argument('--acc', type=int, default=512)
    ap.add_argument('--cam', default='', help='px,py,pz,tx,ty,tz')
    a = ap.parse_args()

    out = f'F:/rtxgi/out/{a.name}'
    script = plan(a.methods, a.ref, a.warm, a.acc)
    if a.cam:
        script = f'15:cam={a.cam};' + script
    print(run.run(out, script, ['-scene', a.scene]))

    ref = img.read_pfm(f'{out}/ref.pfm')
    dst = os.path.join(ROOT, 'results', 'rtxgi', a.name)
    os.makedirs(dst, exist_ok=True)
    img.write_png(os.path.join(dst, 'reference.png'), img.tonemap(ref))
    gpu = {}
    if os.path.exists(f'{out}/fps.txt'):
        lines = [l for l in open(f'{out}/fps.txt').read().splitlines() if l.strip()]
        for name, line in zip(a.methods, lines):
            gpu[name] = float(line.split('gpu_ms')[1].split()[0])
    res = {}
    for name in a.methods:
        acc = img.read_pfm(f'{out}/{name}_acc.pfm')
        nrd = img.read_pfm(f'{out}/{name}_nrd.pfm')
        res[name] = {
            'biais_psnr': img.psnr(acc, ref), 'biais_relmse': img.rel_mse(acc, ref),
            'nrd_psnr': img.psnr(nrd, ref), 'nrd_relmse': img.rel_mse(nrd, ref),
            'lum': float(acc.mean() / ref.mean()), 'gpu_ms': gpu.get(name, 0.0),
        }
        img.write_png(os.path.join(dst, f'{name}_nrd.png'), img.tonemap(nrd))
        img.write_png(os.path.join(dst, f'{name}_erreur.png'), img.error_map(acc, ref))
        r = res[name]
        print(f'{name:9s} biais PSNR {r["biais_psnr"]:.2f} relMSE {r["biais_relmse"]:.4f} lum {r["lum"]:.3f} | '
              f'frame NRD PSNR {r["nrd_psnr"]:.2f} relMSE {r["nrd_relmse"]:.4f} | GPU {r["gpu_ms"]:.2f} ms')
    with open(os.path.join(dst, 'mesures.json'), 'w') as f:
        json.dump({'reglages': vars(a), 'resultats': res}, f, indent=1)


if __name__ == '__main__':
    main()
