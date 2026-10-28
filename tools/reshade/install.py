"""Installe (ou retire) ReShade + l'effet KRC GI dans un jeu, de façon réversible.

L'effet est lu directement dans F:/NewrAYTRACING/reshade : une modification du fichier se voit en
jeu après « Recharger » dans le menu de ReShade (touche Origine), sans rien recopier.

  python -I tools/reshade/install.py --game "F:/Red Dead Redemption 2" --dll <ReShade64.dll>
  python -I tools/reshade/install.py --game "F:/Red Dead Redemption 2" --remove

Si OptiScaler est présent (OptiScaler.ini), ReShade est chargé par lui (ReShade64.dll +
LoadReshade = true, fichier d'origine sauvegardé) ; sinon ReShade est posé en dxgi.dll.
Ne jamais utiliser en ligne (anti-triche).
"""

import argparse
import os
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EFFECTS = os.path.join(ROOT, 'reshade')
MARK = '; installé par KRC (tools/reshade/install.py)'
BACKUP = 'OptiScaler.ini.avant_krc'

RESHADE_INI = f'''{MARK}
[GENERAL]
EffectSearchPaths={EFFECTS}
TextureSearchPaths={EFFECTS}
PresetPath=.\\ReShadePreset.ini
PerformanceMode=0

[INPUT]
KeyOverlay=36,0,0,0

[OVERLAY]
TutorialProgress=4
'''

PRESET = '''Techniques=KRC_GI@KRC_GI.fx
TechniqueSorting=KRC_GI@KRC_GI.fx
'''


def set_load_reshade(ini, value):
    lines = open(ini, encoding='utf-8', errors='surrogateescape').read().split('\n')
    for i, l in enumerate(lines):
        if l.strip().startswith('LoadReshade'):
            lines[i] = f'LoadReshade = {value}'
            break
    else:
        raise SystemExit('LoadReshade introuvable dans OptiScaler.ini')
    open(ini, 'w', encoding='utf-8', errors='surrogateescape', newline='').write('\n'.join(lines))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', required=True)
    ap.add_argument('--dll', help='ReShade64.dll (version sans add-ons)')
    ap.add_argument('--remove', action='store_true')
    a = ap.parse_args()

    game = a.game
    opti = os.path.join(game, 'OptiScaler.ini')
    dll_name = 'ReShade64.dll' if os.path.exists(opti) else 'dxgi.dll'
    dll = os.path.join(game, dll_name)
    ini = os.path.join(game, 'ReShade.ini')
    preset = os.path.join(game, 'ReShadePreset.ini')

    if a.remove:
        for f in (dll, ini, preset):
            if os.path.exists(f) and (f != ini or MARK in open(f, encoding='utf-8', errors='replace').read()):
                os.remove(f)
                print('retiré', f)
        if os.path.exists(os.path.join(game, BACKUP)):
            shutil.copyfile(os.path.join(game, BACKUP), opti)
            os.remove(os.path.join(game, BACKUP))
            print('OptiScaler.ini restauré')
        return

    if not a.dll:
        raise SystemExit('--dll requis')
    for f in (dll, ini, preset):
        if os.path.exists(f) and not (f == ini and MARK in open(f, encoding='utf-8', errors='replace').read()):
            if f == dll and open(f, 'rb').read() == open(a.dll, 'rb').read():
                continue
            raise SystemExit(f'{f} existe déjà et n\'est pas de nous : rien n\'est modifié')
    if os.path.exists(opti):
        if not os.path.exists(os.path.join(game, BACKUP)):
            shutil.copyfile(opti, os.path.join(game, BACKUP))
        set_load_reshade(opti, 'true')
        print('OptiScaler.ini : LoadReshade = true (sauvegarde', BACKUP + ')')
    shutil.copyfile(a.dll, dll)
    open(ini, 'w', encoding='utf-8').write(RESHADE_INI)
    if not os.path.exists(preset):
        open(preset, 'w', encoding='utf-8').write(PRESET)
    print('installé :', dll, ini, preset)
    print('effets lus dans', EFFECTS)


if __name__ == '__main__':
    sys.exit(main())
