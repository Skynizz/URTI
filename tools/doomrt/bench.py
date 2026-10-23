"""Banc de mesure dans Doom RT (PrBoom+ RT, RTGL1 branche krc) : une session par carte, figée par la
touche Pause, où les méthodes passent l'une après l'autre sur la même image.

Pour chaque méthode : préchauffage, mesure des fps, image « avant », coupure d'un groupe de
lampes (light=S), images à +1, +2, +4 ... frames, puis la lumière revient. La référence (mode 3,
chemins complets sans cache) est accumulée sur des centaines de frames, avant et après la coupure.

  python -I tools/doomrt/bench.py --maps 1 2 3 --group half --scale 0.1
Sorties : results/doomrt/<nom>/E1M<n>/ (images PNG choisies) et results/doomrt/<nom>/mesures.json
"""

import argparse
import json
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ppm  # noqa: E402

GAME = 'F:/doom-rt-krc'
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
NAMES = {0: 'jeu', 1: 'sharc', 2: 'krc'}
AFTER = [1, 2, 4, 8, 16, 32, 64, 128, 250]


def plan(methods, warm, scale, acc):
    """Scénario (KRC_SCRIPT), captures (KRC_CAPTURE) et rôle de chaque capture."""
    script = ['100:pause', '101:expo']
    captures, roles = [], {}
    f = 110
    for m in methods:
        script.append(f'{f}:mode={m}')
        script.append(f'{f + warm - 210}:fps=200')
        before = f + warm - 1
        captures.append(before)
        roles[before] = (m, 'avant')
        cut = f + warm
        script.append(f'{cut}:light={scale}')
        for k in AFTER:
            captures.append(cut + k)
            roles[cut + k] = (m, k)
        script.append(f'{cut + AFTER[-1] + 5}:light=1')
        f = cut + AFTER[-1] + 10
    # référence : chemins complets, accumulés (lumière normale puis coupée)
    script.append(f'{f}:mode=3')
    captures.append(f'{f + 300}+{acc}')
    roles[f + 300] = (3, 'avant')
    cut = f + 300 + acc + 5
    script.append(f'{cut}:light={scale}')
    captures.append(f'{cut + 300}+{acc}')
    roles[cut + 300] = (3, 'apres')
    script.append(f'{cut + 300 + acc + 5}:light=1')
    return script, captures, roles


def run_session(mapnum, out, script, captures, env_extra, timeout):
    if os.path.isdir(out):
        shutil.rmtree(out)
    os.makedirs(out)
    env = dict(os.environ)
    env.update({
        'KRC_MODE': '0',
        'KRC_SCRIPT': ';'.join(script),
        'KRC_CAPTURE': ','.join(str(c) for c in captures),
        'KRC_CAPTURE_DIR': out,
        'KRC_EXIT_AFTER': '1',
    })
    env.update(env_extra)
    args = [os.path.join(GAME, 'prboom-plus.exe'), '-iwad', 'doom1.wad', '-warp', '1', str(mapnum), '-skill', '3',
            '-nomonsters', '-nomouse', '-nosound', '-nomusic', '-width', '1280', '-height', '720', '-window']
    p = subprocess.Popen(args, cwd=GAME, env=env)
    try:
        p.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        p.kill()
        print(f'  E1M{mapnum} : délai dépassé')


def fps_lines(out, methods):
    path = os.path.join(out, 'fps.txt')
    if not os.path.exists(path):
        return {}
    lines = open(path).read().split('\n')
    res = {}
    for m, line in zip(methods, [l for l in lines if l.strip()]):
        w = line.split()
        res[NAMES[m]] = {w[i]: float(w[i + 1]) for i in range(4, len(w) - 1, 2)}
    return res


def analyse(out, roles, methods):
    img = lambda f: ppm.read(os.path.join(out, f'f{f:05d}.ppm'))
    ref = {k: img(f) for f, (m, k) in roles.items() if m == 3}
    res = {}
    for m in methods:
        frames = {k: f for f, (mm, k) in roles.items() if mm == m}
        r = {'avant': ppm.psnr(img(frames['avant']), ref['avant'])}
        r['apres'] = {k: ppm.psnr(img(frames[k]), ref['apres']) for k in AFTER}
        res[NAMES[m]] = r
    return res


def keep_images(out, dst, roles):
    """Images utiles en PNG (avant, +4, +32, +250 et références)."""
    os.makedirs(dst, exist_ok=True)
    for f, (m, k) in roles.items():
        if k in ('avant', 'apres', 4, 32, 250):
            name = f'{NAMES.get(m, "reference")}_{k}.png'
            subprocess.run(['ffmpeg', '-loglevel', 'error', '-y', '-i', os.path.join(out, f'f{f:05d}.ppm'),
                            os.path.join(dst, name)], check=False)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--maps', type=int, nargs='+', default=[1])
    ap.add_argument('--methods', type=int, nargs='+', default=[0, 1, 2])
    ap.add_argument('--group', default='all', choices=['all', 'half'])
    ap.add_argument('--scale', type=float, default=0.1)
    ap.add_argument('--warm', type=int, default=400)
    ap.add_argument('--acc', type=int, default=512)
    ap.add_argument('--name', default='coupure')
    ap.add_argument('--env', nargs='*', default=[], help='réglages en plus, NOM=valeur')
    ap.add_argument('--timeout', type=int, default=900)
    a = ap.parse_args()

    script, captures, roles = plan(a.methods, a.warm, a.scale, a.acc)
    script.insert(2, f'102:group={a.group}')
    env = dict(e.split('=', 1) for e in a.env)
    results_dir = os.path.join(ROOT, 'results', 'doomrt', a.name)
    os.makedirs(results_dir, exist_ok=True)
    summary = {'reglages': vars(a), 'cartes': {}}
    for mp in a.maps:
        out = os.path.join(GAME, 'cap', a.name, f'E1M{mp}')
        print(f'E1M{mp} ...', flush=True)
        run_session(mp, out, script, captures, env, a.timeout)
        try:
            res = analyse(out, roles, a.methods)
        except FileNotFoundError as e:
            print('  capture manquante :', e)
            continue
        res['fps'] = fps_lines(out, a.methods)
        summary['cartes'][f'E1M{mp}'] = res
        keep_images(out, os.path.join(results_dir, f'E1M{mp}'), roles)
        for name, r in res.items():
            if name == 'fps':
                continue
            series = ' '.join(f'+{k}:{v:.1f}' for k, v in r['apres'].items())
            t = res['fps'].get(name, {})
            print(f'  {name:6s} avant {r["avant"]:.2f} dB | après coupure {series} | GPU {t.get("gpu", 0):.2f} ms '
                  f'(cache {t.get("cache", 0):.2f}, indirect {t.get("indirect", 0):.2f}) {t.get("fps", 0):.0f} fps')
    with open(os.path.join(results_dir, 'mesures.json'), 'w') as f:
        json.dump(summary, f, indent=1)


if __name__ == '__main__':
    main()
