"""Réaction à un changement de lumière dans le path tracer d'exemple de NVIDIA (branche krc).

Une session invisible : références A et B (chemins complets accumulés, jitter coupé), puis pour
chaque méthode et chaque débruiteur (NRD, aucun) : convergence avec l'éclairage A, bascule vers B,
captures à +k frames. Mesures contre la référence B : ꟻLIP (frames NRD) et écart de luminance
moyenne hors ciel (le retard du cache, lisible même sur une frame brute).

  python -I tools/rtxgi/event.py --scene LivingRoom.scene.json --name salon_soleil \\
      --before sun=20 --after sun=5 --methods sharc krcl
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
    'sharc': ['tech=sharc', 'scale=50', 'guide=0', 'relight=0', 'nrdconf=0', 'nrdaf=1'],
    'sharc_vide': ['tech=sharc', 'scale=50', 'guide=0', 'relight=0', 'nrdconf=0', 'nrdaf=1'],  # cache vidé à la bascule
    'sharc_nrdreset': ['tech=sharc', 'scale=50', 'guide=0', 'relight=0', 'nrdconf=0', 'nrdaf=1'],  # NRD vidé à la bascule
    'krcl': ['tech=sharc', 'scale=50', 'guide=0', 'relight=1', 'nrdconf=0', 'nrdaf=1', 'krcdebug=0'],  # SHaRC + rééclairage exact
    'krcl_conf': ['tech=sharc', 'scale=50', 'guide=0', 'relight=1', 'nrdconf=1', 'nrdaf=1', 'krcdebug=0'],  # + confiance de NRD
    'krcl_norm': ['tech=sharc', 'scale=50', 'guide=0', 'relight=1', 'nrdconf=2', 'nrdaf=1', 'krcdebug=0'],  # + historique de NRD rééclairé
    'krcl_conf0': ['tech=sharc', 'scale=50', 'guide=0', 'relight=1', 'nrdconf=1', 'nrdaf=1', 'krcdebug=3'],  # diagnostic
    'krcal_norm': ['tech=krc', 'scale=50', 'guide=0', 'relight=1', 'nrdconf=2', 'nrdaf=1', 'krcdebug=0'],  # KRC-A + rééclairage
}
AT_SWITCH = {'sharc_vide': ['clear'], 'sharc_nrdreset': ['nrdreset']}
OFFSETS = [0, 1, 2, 4, 8, 16, 32, 64, 128, 256, 512]


def plan(methods, setup, before, after, ref_spp, warm):
    cmds = [f'20:{c}' for c in setup] + ['20:jitter=0', '20:tech=ref', '20:den=acc']
    f = 20
    for name, state in (('refA', before), ('refB', after)):
        cmds += [f'{f}:{c}' for c in state + ['den=acc']]
        cmds.append(f'{f + ref_spp}:capture={name}')
        f += ref_spp + 5
    for name in methods:
        for den in ('nrd', 'none'):
            cmds += [f'{f}:{c}' for c in ['krcdebug=0'] + METHODS[name] + before + [f'den={den}']]
            t0 = f + warm
            cmds += [f'{t0}:{c}' for c in after]
            cmds += [f'{t0}:{c}' for c in AT_SWITCH.get(name, [])]
            cmds += [f'{t0 + k}:capture={name}_{den}_{k}' for k in OFFSETS]
            f = t0 + OFFSETS[-1] + 5
    cmds.append(f'{f}:exit')
    return ';'.join(cmds)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--scene', default='LivingRoom.scene.json')
    ap.add_argument('--name', default='salon_soleil')
    ap.add_argument('--setup', nargs='*', default=[], help='commandes du début (lampes ajoutées, groupes)')
    ap.add_argument('--before', nargs='+', default=['sun=20'])
    ap.add_argument('--after', nargs='+', default=['sun=5'])
    ap.add_argument('--methods', nargs='+', default=['sharc', 'krcl'])
    ap.add_argument('--ref', type=int, default=2048)
    ap.add_argument('--warm', type=int, default=300)
    ap.add_argument('--eval-only', action='store_true')
    a = ap.parse_args()

    out = f'F:/rtxgi/out/evt_{a.name}'
    if not a.eval_only:
        print(run.run(out, plan(a.methods, a.setup, a.before, a.after, a.ref, a.warm), ['-scene', a.scene]))

    L = lambda x: x[..., :3] @ np.array([0.2126, 0.7152, 0.0722])
    refA, refB = img.read_pfm(f'{out}/refA.pfm'), img.read_pfm(f'{out}/refB.pfm')
    mask = L(refA) < 5.0  # hors ciel
    lb = L(refB)[mask].mean()
    print(f'changement de luminance hors ciel A -> B : {L(refA)[mask].mean() / lb:.3f}')
    dst = os.path.join(ROOT, 'results', 'rtxgi', a.name)
    os.makedirs(dst, exist_ok=True)
    img.write_png(os.path.join(dst, 'reference_avant.png'), img.tonemap(refA))
    img.write_png(os.path.join(dst, 'reference_apres.png'), img.tonemap(refB))

    res = {}
    for name in a.methods:
        r = {'offsets': OFFSETS, 'nrd_flip': [], 'nrd_lum': [], 'brut_lum': []}
        for k in OFFSETS:
            nrd = img.read_pfm(f'{out}/{name}_nrd_{k}.pfm')
            raw = img.read_pfm(f'{out}/{name}_none_{k}.pfm')
            r['nrd_flip'].append(img.flip(nrd, refB)[0])
            r['nrd_lum'].append(float(L(nrd)[mask].mean() / lb))
            r['brut_lum'].append(float(L(raw)[mask].mean() / lb))
            if k in (0, 4, 32):
                img.write_png(os.path.join(dst, f'{name}_nrd_{k}.png'), img.tonemap(nrd))
        res[name] = r
        print(f'{name:11s} NRD FLIP ' + ' '.join(f'{v:.3f}' for v in r['nrd_flip']))
        print(f'{"":11s} NRD lum  ' + ' '.join(f'{v:.3f}' for v in r['nrd_lum']))
        print(f'{"":11s} brut lum ' + ' '.join(f'{v:.3f}' for v in r['brut_lum']), flush=True)
    print('frames : ' + ' '.join(str(k) for k in OFFSETS))
    with open(os.path.join(dst, 'evenement.json'), 'w') as f:
        json.dump({'reglages': vars(a), 'resultats': res}, f, indent=1)


if __name__ == '__main__':
    main()
