"""Vues fixes dans une carte de Quake II RTX (branche krc) : captures de chaque méthode, et
référence du jeu par accumulation en pause.

    python tools/q2rtx/views.py --map demo1 --view "420 480 -180 12 90" --modes 0 1 2 --ref
    python tools/q2rtx/views.py --scout --map demo1 --view "..." --view "..."   # repérage (jeu seul)

Sorties : results/q2rtx/vues/<nom>/m<méthode>.png, ref.png, comparaison.png, mesures.txt.
"""
import argparse
import os
import shutil
import subprocess

GAME = 'F:/Q2RTX-KRC'
OUT = os.path.join(os.path.dirname(__file__), '..', '..', 'results', 'q2rtx', 'vues')
NAMES = {0: 'Jeu sans cache', 1: 'SHaRC NVIDIA', 2: 'KRC', 'ref': 'Reference'}


def run(cfg_name, lines):
    cfg = os.path.join(GAME, 'baseq2', cfg_name)
    with open(cfg, 'w') as f:
        f.write('\n'.join(lines) + '\n')
    subprocess.run([os.path.join(GAME, 'q2rtx.exe'), '+set', 'vid_fullscreen', '0', '+set', 'vid_geometry',
                    '1280x720', '+exec', cfg_name], cwd=GAME, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                   timeout=1200)
    os.remove(cfg)


def setup(mapname):
    # carte chargée, joueur invisible pour les monstres, sans gravité ; arme et ATH masqués, souris
    # et clavier coupés (la fenêtre du jeu prend la souris : un geste ferait tourner la vue)
    return ['set pt_accumulation_rendering 0', 'set cheats 1', f'map {mapname}', 'wait 120',
            'set hand 2', 'set in_enable 0', 'set scr_draw2d 0', 'set con_notifytime 0', 'god', 'notarget', 'noclip', 'wait 10']


def shots():
    d = os.path.join(GAME, 'baseq2', 'screenshots')
    os.makedirs(d, exist_ok=True)
    return d


def capture(mapname, views, modes, ref, spp, warm, extra, accum=False):
    # une seule session par vue, jeu en pause : toutes les méthodes et la référence voient
    # exactement le même instant (l'eau ondule, les objets tournent)
    for i, v in enumerate(views):
        # exposition figée après la pause : même exposition pour toutes les captures
        lines = setup(mapname) + [*extra, f'krc_view {v}', 'wait 30', 'pause', 'wait 60',
                                  'set tm_exposure_speed_up 0; set tm_exposure_speed_down 0']
        for m in modes:
            lines += [f'set pt_cache {m}', f'wait {warm}', f'screenshotpng kv_{i}_m{m}']
            if accum:
                # biais seul : la méthode accumulée comme la référence (même pipeline, sans débruiteur)
                lines += ['set pt_dof 0', f'set pt_accumulation_rendering_framenum {spp // 2}',
                          'set pt_accumulation_rendering 1', f'wait {spp // 2 + 30}', f'screenshotpng kv_{i}_a{m}',
                          'set pt_accumulation_rendering 0', 'wait 10']
        if ref:
            # référence : chemins complets à 8 rebonds sans cache (pt_cache 3), accumulés
            lines += ['set pt_cache 3', 'set pt_dof 0', 'set pt_fake_roughness_threshold 1', f'set pt_accumulation_rendering_framenum {spp}',
                      'set pt_accumulation_rendering 1', f'wait {spp + 30}', f'screenshotpng kv_{i}_mref']
        lines += ['quit']
        run(f'kv{i}.cfg', lines)
        d = os.path.join(OUT, f'{mapname}_{i}')
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, 'vue.txt'), 'w') as f:
            f.write(f'map {mapname}\nkrc_view {v}\n')
        for m in list(modes) + (['ref'] if ref else []):
            for pre in ['m', 'a']:
                src = os.path.join(shots(), f'kv_{i}_{pre}{m}.png')
                if os.path.exists(src):
                    os.replace(src, os.path.join(d, f'{pre}{m}.png'))


def psnr(a, b):
    r = subprocess.run(['ffmpeg', '-i', a, '-i', b, '-lavfi', 'psnr', '-f', 'null', '-'], capture_output=True, text=True)
    for tok in r.stderr.split():
        if tok.startswith('average:'):
            return tok.split(':')[1]
    return '?'


def compose(d, keys):
    imgs = [os.path.join(d, f'm{k}.png') for k in keys if os.path.exists(os.path.join(d, f'm{k}.png'))]
    keys = [k for k in keys if os.path.exists(os.path.join(d, f'm{k}.png'))]
    if not imgs:
        return
    cmd = ['ffmpeg', '-y', '-loglevel', 'error']
    for p in imgs:
        cmd += ['-i', p]
    filt = ''.join(f"[{i}:v]scale=640:-1,drawtext=fontfile='C\\:/Windows/Fonts/arial.ttf':text='{NAMES[k]}':x=10:y=10:"
                   f"fontsize=20:fontcolor=white:box=1:boxcolor=black@0.6:boxborderw=5[v{i}];" for i, k in enumerate(keys))
    filt += ''.join(f'[v{i}]' for i in range(len(keys))) + (f'hstack=inputs={len(keys)}' if len(keys) > 1 else 'null')
    subprocess.run(cmd + ['-filter_complex', filt, os.path.join(d, 'comparaison.png')], check=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--map', default='demo1')
    ap.add_argument('--view', action='append', required=True, help='"x y z pitch yaw"')
    ap.add_argument('--modes', type=int, nargs='+', default=[0, 1, 2])
    ap.add_argument('--ref', action='store_true')
    ap.add_argument('--scout', action='store_true')
    ap.add_argument('--accum', action='store_true', help='aussi chaque méthode accumulée (biais seul)')
    ap.add_argument('--spp', type=int, default=1024)
    ap.add_argument('--warm', type=int, default=240, help='frames avant la capture (cache rempli)')
    ap.add_argument('--set', nargs='*', default=[])
    a = ap.parse_args()
    extra = [f'set {kv.split("=")[0]} {kv.split("=")[1]}' for kv in a.set]
    modes = [0] if a.scout else a.modes
    capture(a.map, a.view, modes, a.ref and not a.scout, a.spp, 60 if a.scout else a.warm, extra, a.accum)
    for i in range(len(a.view)):
        d = os.path.join(OUT, f'{a.map}_{i}')
        keys = list(modes) + (['ref'] if a.ref and not a.scout else [])
        compose(d, keys)
        if a.ref and not a.scout:
            ref = os.path.join(d, 'mref.png')
            lines = [f'{NAMES[m]} : PSNR {psnr(os.path.join(d, f"m{m}.png"), ref)} dB' +
                     (f' (accumulé : {psnr(os.path.join(d, f"a{m}.png"), ref)} dB)' if a.accum else '') for m in modes]
            with open(os.path.join(d, 'mesures.txt'), 'w', encoding='utf8') as f:
                f.write('\n'.join(lines) + '\n')
            print(f'{a.map}_{i} : ' + ' | '.join(lines), flush=True)


if __name__ == '__main__':
    main()
