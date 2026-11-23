"""URTI: stability of the computation, from a test trace (urti.ini frame_trace=1, journal.txt of a scripted test run).

    python urti_trace_ana.py <folder>                        summary per phase
    python urti_trace_ana.py <folder> <phase> <first> <last> detail, computation by computation

A row of depth and mask is read back at each computation. With a still camera, its mean depth and its mean mask must not
move: a jump means the computation saw a half-drawn frame (v77 test: that is what showed as "effect / no
effect", while the "300 presents out of 300" summaries stayed perfect).
"""
import re
import statistics as st
import sys

RX = re.compile(r'FRAME (\d+) t ([\d.]+) compute gen (\d) draws (\d+) main (\d+) regions (\d+) places (\d+) probes (\d+) cam (\d+)')
RX0 = re.compile(r'FRAME (\d+) t ([\d.]+) no compute')
RR = re.compile(r'ROW (\d+) sky ([\d.]+) depth ([\d.]+) mask ([\d.]+)')


def summary(folder):
    phase = 'before'
    computations = {}
    per_frame = {}
    for line in open(folder + '/journal.txt', encoding='utf-8', errors='replace'):
        if line.startswith('PHASE '):
            phase = line.split()[1]
            continue
        m = RX.search(line)
        if m:
            d = dict(zip('f t gen draws main zones places probes cam'.split(), map(float, m.groups())))
            per_frame[int(d['f'])] = d
            computations.setdefault(phase, []).append(d)
            continue
        m = RR.search(line)
        if m and int(m[1]) in per_frame:
            per_frame[int(m[1])].update(sky=float(m[2]), dep=float(m[3]), lit=float(m[4]))
    print("phase, computations | regions of the frame's scene | nearly empty rows | mask: jumps above 3 %, above 8 %")
    for ph, c in computations.items():
        if len(c) < 20 or ph == 'before':
            continue
        zones = [x['zones'] for x in c]
        lit = [x['lit'] for x in c if 'lit' in x]
        jumps = [abs(q - p) for p, q in zip(lit, lit[1:])]
        print(f"{ph:15} {len(c):5d} | median {st.median(zones):5.0f}, empty {sum(1 for v in zones if v == 0):4d} | "
              f"{sum(1 for x in c if x.get('sky', 0) > 5):4d} | {sum(1 for v in jumps if v > .03):4d} {sum(1 for v in jumps if v > .08):4d}")


def detail(folder, want, first, last):
    phase = 'before'
    rows = []
    per_frame = {}
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
            per_frame[d['f']] = d
            if phase == want:
                rows.append(d)
            continue
        m = RR.search(line)
        if m and int(m[1]) in per_frame:
            per_frame[int(m[1])].update(sky=float(m[2]), dep=float(m[3]), lit=float(m[4]))
    print('   ms executed regions places probes camera   sky      depth    mask')
    before = None
    for d in rows[first:last]:
        dt = d['t'] - before if before else 0
        before = d['t']
        if not d['calc']:
            print(f'{dt:5.1f}   -')
            continue
        print(f"{dt:5.1f} {d['draws']:8d} {d['zones']:7d} {d['places']:6d} {d['probes']:6d} {d['cam']:6d} {d.get('sky', -1):5.1f} {d.get('dep', -1):10.5f} {d.get('lit', -1):7.4f}")


if len(sys.argv) >= 5:
    detail(sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]))
else:
    summary(sys.argv[1])
