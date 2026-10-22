"""Captures et mesures dans Quake II RTX (branche krc de F:/Q2RTX-KRC).

Rejoue la démo q2demo1 en timedemo : chaque image rendue avance d'une frame de démo, donc
une même frame montre exactement la même scène pour chaque méthode.

    python tools/q2rtx/capture.py                 # captures + i/s, méthodes 0 1 2
    python tools/q2rtx/capture.py --modes 0 1     # méthodes choisies
    python tools/q2rtx/capture.py --fps-only

Méthodes (cvar pt_cache) : 0 jeu d'origine, 1 SHaRC (SDK NVIDIA), 2 KRC.
Sorties : results/q2rtx/m<méthode>/fNNN.png, results/q2rtx/fps.txt, comparaisons côte à côte.
"""
import argparse
import os
import re
import shutil
import subprocess

GAME = 'F:/Q2RTX-KRC'
OUT = os.path.join(os.path.dirname(__file__), '..', '..', 'results', 'q2rtx')
NAMES = {0: 'Jeu sans cache', 1: 'SHaRC NVIDIA', 2: 'KRC'}
FRAMES = [120, 270, 420, 560]


def run_game(cfg_name, lines, extra=()):
    cfg = os.path.join(GAME, 'baseq2', cfg_name)
    with open(cfg, 'w') as f:
        f.write('\n'.join(lines) + '\n')
    args = [os.path.join(GAME, 'q2rtx.exe'), '+set', 'vid_fullscreen', '0', '+set', 'vid_geometry', '1280x720',
            *extra, '+exec', cfg_name]
    subprocess.run(args, cwd=GAME, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=900)
    os.remove(cfg)


def capture(mode, frames, settings):
    shots = os.path.join(GAME, 'baseq2', 'screenshots')
    os.makedirs(shots, exist_ok=True)
    lines = ['set timedemo 1', f'set pt_cache {mode}', *settings, 'demo q2demo1']
    prev = 0
    for f in frames:
        lines += [f'wait {f - prev}', f'screenshotpng krc_m{mode}_f{f:03d}']
        prev = f
    lines += ['wait 5', 'quit']
    run_game(f'krc_cap{mode}.cfg', lines)
    dst = os.path.join(OUT, f'm{mode}')
    os.makedirs(dst, exist_ok=True)
    for f in frames:
        src = os.path.join(shots, f'krc_m{mode}_f{f:03d}.png')
        if os.path.exists(src):
            shutil.move(src, os.path.join(dst, f'f{f:03d}.png'))


def fps(mode, settings):
    log = f'krc_fps{mode}'
    run_game(f'krc_fps{mode}.cfg', ['set timedemo 1', f'set pt_cache {mode}', *settings,
                                    f'set logfile_name {log}', 'set logfile_flush 1', 'set logfile 1',
                                    'set nextserver quit', 'demo q2demo1'])
    text = open(os.path.join(GAME, 'baseq2', 'logs', log + '.log'), encoding='utf8', errors='replace').read()
    m = re.findall(r'(\d+) frames, ([\d.]+) seconds: ([\d.]+) fps', text)
    return m[-1] if m else None


def side_by_side(modes, frames):
    for f in frames:
        inputs = [os.path.join(OUT, f'm{m}', f'f{f:03d}.png') for m in modes]
        if not all(os.path.exists(p) for p in inputs):
            continue
        filt = ''.join(
            f"[{i}:v]scale=960:-1,drawtext=fontfile='C\\:/Windows/Fonts/arial.ttf':text='{NAMES[m]}':"
            f"x=12:y=12:fontsize=24:fontcolor=white:box=1:boxcolor=black@0.55:boxborderw=6[v{i}];"
            for i, m in enumerate(modes))
        filt += ''.join(f'[v{i}]' for i in range(len(modes))) + f'hstack=inputs={len(modes)}'
        cmd = ['ffmpeg', '-y', '-loglevel', 'error']
        for p in inputs:
            cmd += ['-i', p]
        cmd += ['-filter_complex', filt, os.path.join(OUT, f'comparaison_f{f:03d}.png')]
        subprocess.run(cmd, check=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--modes', type=int, nargs='+', default=[0, 1, 2])
    ap.add_argument('--frames', type=int, nargs='+', default=FRAMES)
    ap.add_argument('--fps-only', action='store_true')
    ap.add_argument('--set', nargs='*', default=[], help='cvars supplémentaires, "nom=valeur"')
    a = ap.parse_args()
    settings = [f'set {kv.split("=")[0]} {kv.split("=")[1]}' for kv in a.set]
    os.makedirs(OUT, exist_ok=True)
    report = []
    for m in a.modes:
        if not a.fps_only:
            capture(m, a.frames, settings)
        r = fps(m, settings)
        line = f'{NAMES[m]} (pt_cache {m}) : ' + (f'{r[0]} frames en {r[1]} s, {float(r[2]):.1f} i/s' if r else 'échec')
        print(line, flush=True)
        report.append(line)
    with open(os.path.join(OUT, 'fps.txt'), 'w', encoding='utf8') as f:
        f.write('\n'.join(report) + '\n')
    if not a.fps_only:
        side_by_side(a.modes, a.frames)


if __name__ == '__main__':
    main()
