"""URTI : stabilité du calcul, d'après une trace d'essai (urti.ini frame_trace=1, journal.txt de urti_essai_trace.sh).

    python urti_trace_ana.py <dossier>                        bilan par phase
    python urti_trace_ana.py <dossier> <phase> <début> <fin>  détail, calcul par calcul

Une rangée de profondeur et de masque est relue à chaque calcul. Caméra immobile, sa profondeur moyenne et son masque moyen ne doivent
pas bouger : un saut veut dire que le calcul a vu une image à moitié dessinée (essai v77 : c'est ce qui se voyait comme « effet / pas
effet », alors que les bilans « 300 présentations sur 300 » restaient parfaits).
"""
import re
import statistics as st
import sys

RX = re.compile(r'TRAME (\d+) t ([\d.]+) calcul gen (\d) dessins (\d+) principal (\d+) zones (\d+) places (\d+) mesures (\d+) cam (\d+)')
RX0 = re.compile(r'TRAME (\d+) t ([\d.]+) sans calcul')
RR = re.compile(r'RANGEE (\d+) ciel ([\d.]+) profondeur ([\d.]+) masque ([\d.]+)')


def bilan(folder):
    phase = 'avant'
    calculs = {}
    par_image = {}
    for line in open(folder + '/journal.txt', encoding='utf-8', errors='replace'):
        if line.startswith('PHASE '):
            phase = line.split()[1]
            continue
        m = RX.search(line)
        if m:
            d = dict(zip('f t gen draws main zones places probes cam'.split(), map(float, m.groups())))
            par_image[int(d['f'])] = d
            calculs.setdefault(phase, []).append(d)
            continue
        m = RR.search(line)
        if m and int(m[1]) in par_image:
            par_image[int(m[1])].update(sky=float(m[2]), dep=float(m[3]), lit=float(m[4]))
    print("phase, calculs | zones de la scène de l'image | rangées presque vides | masque : sauts de plus de 3 %, de plus de 8 %")
    for ph, c in calculs.items():
        if len(c) < 20 or ph == 'avant':
            continue
        zones = [x['zones'] for x in c]
        lit = [x['lit'] for x in c if 'lit' in x]
        sauts = [abs(q - p) for p, q in zip(lit, lit[1:])]
        print(f"{ph:15} {len(c):5d} | médiane {st.median(zones):5.0f}, vides {sum(1 for v in zones if v == 0):4d} | "
              f"{sum(1 for x in c if x.get('sky', 0) > 5):4d} | {sum(1 for v in sauts if v > .03):4d} {sum(1 for v in sauts if v > .08):4d}")


def detail(folder, want, first, last):
    phase = 'avant'
    rows = []
    par_image = {}
    for line in open(folder + '/journal.txt', encoding='utf-8', errors='replace'):
        if line.startswith('PHASE '):
            phase = line.split()[1]
            continue
        m = RX0.search(line)
        if m and phase == want:
            rows.append(dict(f=int(m[1]), t=float(m[2]), calc=0))
            continue
        m = RX.search(line)
        if m:
            d = dict(f=int(m[1]), t=float(m[2]), calc=1, draws=int(m[4]), zones=int(m[6]), places=int(m[7]), probes=int(m[8]), cam=int(m[9]))
            par_image[d['f']] = d
            if phase == want:
                rows.append(d)
            continue
        m = RR.search(line)
        if m and int(m[1]) in par_image:
            par_image[int(m[1])].update(sky=float(m[2]), dep=float(m[3]), lit=float(m[4]))
    print('   ms exécutés zones places mesures caméra  ciel profondeur  masque')
    before = None
    for d in rows[first:last]:
        dt = d['t'] - before if before else 0
        before = d['t']
        if not d['calc']:
            print(f'{dt:5.1f}   -')
            continue
        print(f"{dt:5.1f} {d['draws']:8d} {d['zones']:5d} {d['places']:6d} {d['probes']:7d} {d['cam']:6d} {d.get('sky', -1):5.1f} {d.get('dep', -1):10.5f} {d.get('lit', -1):7.4f}")


if len(sys.argv) >= 5:
    detail(sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]))
else:
    bilan(sys.argv[1])
