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
    ap.add_argument('--dll', help='ReShade64.dll (version sans add-ons, ou « Addon » avec --addon)')
    ap.add_argument('--addon', action='append', default=[], help='add-on .addon64 à poser à côté (URTI)')
    ap.add_argument('--no-effects', action='store_true', help="aucun effet actif (mesures d'URTI)")
    ap.add_argument('--remove', action='store_true')
    a = ap.parse_args()

    game = a.game
    opti = os.path.join(game, 'OptiScaler.ini')
    dll_name = 'ReShade64.dll' if os.path.exists(opti) else 'dxgi.dll'
    dll = os.path.join(game, dll_name)
    ini = os.path.join(game, 'ReShade.ini')
    preset = os.path.join(game, 'ReShadePreset.ini')

    # nôtre si notre marque y est, ou notre dossier d'effets (ReShade réécrit son fichier et ôte la marque)
    ini_text = open(ini, encoding='utf-8', errors='replace').read() if os.path.exists(ini) else ''
    ours = MARK in ini_text or EFFECTS.lower() in ini_text.lower()
    addons = [os.path.join(game, n) for n in os.listdir(game) if n.endswith('.addon64') and n.startswith('urti')]
    if a.remove:
        for f in [dll, ini, preset] + addons:
            if os.path.exists(f) and ours:
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
        if os.path.exists(f) and not ours:
            raise SystemExit(f'{f} existe déjà et n\'est pas de nous : rien n\'est modifié')
    if os.path.exists(opti):
        if not os.path.exists(os.path.join(game, BACKUP)):
            shutil.copyfile(opti, os.path.join(game, BACKUP))
        set_load_reshade(opti, 'true')
        print('OptiScaler.ini : LoadReshade = true (sauvegarde', BACKUP + ')')
    shutil.copyfile(a.dll, dll)
    open(ini, 'w', encoding='utf-8').write(RESHADE_INI)
    open(preset, 'w', encoding='utf-8').write('' if a.no_effects else PRESET)
    for f in addons:
        os.remove(f)
    for f in a.addon:
        shutil.copyfile(f, os.path.join(game, os.path.basename(f)))
    print('installé :', dll, ini, preset, *[os.path.basename(f) for f in a.addon])
    print('effets lus dans', EFFECTS)


if __name__ == '__main__':
    sys.exit(main())
