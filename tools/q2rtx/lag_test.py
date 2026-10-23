"""Test de réaction à un changement de lumière dans Quake II RTX (branche krc).

La démo q2demo1 est figée (pause) à une frame donnée : caméra et scène immobiles. Après
convergence, toutes les lampes passent brusquement à SCALE de leur intensité
(pt_test_light_scale, « coupure de courant ») ; on capture les frames suivantes, puis, dans
la même session et donc avec la même caméra, une référence par le rendu de référence du jeu
(accumulation en pause).

    python tools/q2rtx/lag_test.py [--frame 300] [--scale 0.15] [--modes 0 1 2]

Sorties : results/q2rtx/reaction/ (images, reaction.png, mesures.txt : PSNR contre la référence).
"""
import argparse
import os
import shutil
import subprocess

GAME = 'F:/Q2RTX-KRC'
OUT = os.path.join(os.path.dirname(__file__), '..', '..', 'results', 'q2rtx', 'reaction')
NAMES = {0: 'Jeu sans cache', 1: 'SHaRC NVIDIA', 2: 'KRC'}
AFTER = [1, 2, 4, 8, 16, 32, 64]


def run(cfg_name, lines):
    cfg = os.path.join(GAME, 'baseq2', cfg_name)
    with open(cfg, 'w') as f:
        f.write('\n'.join(lines) + '\n')
    subprocess.run([os.path.join(GAME, 'q2rtx.exe'), '+set', 'vid_fullscreen', '0', '+set', 'vid_geometry',
                    '1280x720', '+exec', cfg_name], cwd=GAME, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                   timeout=900)
    os.remove(cfg)


def shots_dir():
    d = os.path.join(GAME, 'baseq2', 'screenshots')
    os.makedirs(d, exist_ok=True)
    return d


def method(mode, frame, scale, extra, spp):
    # le moteur ignore la pause en timedemo : on quitte le timedemo au moment de figer la démo
    lines = ['set timedemo 1', 'set pt_accumulation_rendering 0', f'set pt_cache {mode}', *extra,
             'demo q2demo1', f'wait {frame}', 'set timedemo 0; pause', 'wait 90', f'screenshotpng rt_m{mode}_avant',
             # exposition figée : l'adaptation de l'œil du jeu ne doit pas se mêler au retard mesuré
             'set tm_exposure_speed_up 0; set tm_exposure_speed_down 0', f'set pt_test_light_scale {scale}']
    prev = 0
    for a in AFTER:
        lines += [f'wait {a - prev}', f'screenshotpng rt_m{mode}_p{a:02d}']
        prev = a
    # référence d'après, même caméra : chemins complets à 8 rebonds sans cache (pt_cache 3), accumulés en pause
    lines += ['set pt_cache 3', 'set pt_dof 0', 'set pt_fake_roughness_threshold 1', f'set pt_accumulation_rendering_framenum {spp}', 'set pt_accumulation_rendering 1',
              f'wait {spp + 30}', f'screenshotpng rt_m{mode}_ref', 'set pt_test_light_scale 1', 'wait 5', 'quit']
    run(f'rt{mode}.cfg', lines)
    dst = os.path.join(OUT, f'm{mode}')
    os.makedirs(dst, exist_ok=True)
    for tag in ['avant', 'ref'] + [f'p{a:02d}' for a in AFTER]:
        src = os.path.join(shots_dir(), f'rt_m{mode}_{tag}.png')
        if os.path.exists(src):
            os.replace(src, os.path.join(dst, f'{tag}.png'))


def psnr(a, b):
    if not (os.path.exists(a) and os.path.exists(b)):
        return '?'
    r = subprocess.run(['ffmpeg', '-i', a, '-i', b, '-lavfi', 'psnr', '-f', 'null', '-'], capture_output=True, text=True)
    for tok in r.stderr.split():
        if tok.startswith('average:'):
            return tok.split(':')[1]
    return '?'


def strip(modes):
    # une ligne par méthode : avant, +1, +4, +16, +64, et la référence d'après
    tags = ['avant', 'p01', 'p04', 'p16', 'p64', 'ref']
    labels = ['avant', '+1 frame', '+4', '+16', '+64', 'reference apres']
    rows = []
    for m in modes:
        imgs = [os.path.join(OUT, f'm{m}', f'{t}.png') for t in tags]
        if not all(os.path.exists(p) for p in imgs):
            continue
        row = os.path.join(OUT, f'ligne_m{m}.png')
        cmd = ['ffmpeg', '-y', '-loglevel', 'error']
        for p in imgs:
            cmd += ['-i', p]
        filt = ''.join(f"[{i}:v]scale=426:-1,drawtext=fontfile='C\\:/Windows/Fonts/arial.ttf':text='{NAMES[m]} {labels[i]}'"
                       f":x=8:y=8:fontsize=16:fontcolor=white:box=1:boxcolor=black@0.6:boxborderw=4[v{i}];"
                       for i in range(len(imgs)))
        filt += ''.join(f'[v{i}]' for i in range(len(imgs))) + f'hstack=inputs={len(imgs)}'
        subprocess.run(cmd + ['-filter_complex', filt, row], check=True)
        rows.append(row)
    if rows:
        cmd = ['ffmpeg', '-y', '-loglevel', 'error']
        for r in rows:
            cmd += ['-i', r]
        out = os.path.join(OUT, 'reaction.png')
        filt = f'vstack=inputs={len(rows)}' if len(rows) > 1 else 'null'
        subprocess.run(cmd + ['-filter_complex', filt, out], check=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--frame', type=int, default=300)
    ap.add_argument('--scale', type=float, default=0.15)
    ap.add_argument('--modes', type=int, nargs='+', default=[0, 1, 2])
    ap.add_argument('--spp', type=int, default=512)
    ap.add_argument('--set', nargs='*', default=[], help='cvars supplémentaires, "nom=valeur"')
    a = ap.parse_args()
    extra = [f'set {kv.split("=")[0]} {kv.split("=")[1]}' for kv in a.set]
    os.makedirs(OUT, exist_ok=True)
    for m in a.modes:
        method(m, a.frame, a.scale, extra, a.spp)
    report = [f"Coupure à {a.scale} à la frame {a.frame} (démo en pause) ; PSNR (dB) contre la référence d'après"]
    for m in a.modes:
        d = os.path.join(OUT, f'm{m}')
        ref = os.path.join(d, 'ref.png')
        line = f'{NAMES[m]} :' + ''.join(f' | +{t} {psnr(os.path.join(d, f"p{t:02d}.png"), ref)}' for t in AFTER)
        report.append(line)
        print(line, flush=True)
    with open(os.path.join(OUT, 'mesures.txt'), 'w', encoding='utf8') as f:
        f.write('\n'.join(report) + '\n')
    strip(a.modes)


if __name__ == '__main__':
    main()
