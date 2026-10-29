"""URTI : analyse hors ligne d'une capture (.urti) : objets retrouvés d'une frame à l'autre, champ de
vision et mouvement de la caméra estimés sans rien savoir du jeu (auto-étalonnage), comparés à la
vérité terrain si elle existe (banc : URTI_TRUTH).

Principe. En espace de découpage, w est la profondeur le long de l'axe de vue. Avec le champ de vision
vertical (P11 = 1 / tan(fov / 2), P00 = P11 × hauteur / largeur), chaque sommet devient un point de
l'espace caméra. Un objet immobile vu dans deux frames doit alors bouger d'un seul mouvement rigide
(rotation + translation) : on cherche le champ de vision pour lequel ce mouvement est le plus rigide.

Soleil. Le jeu dessine aussi la scène vue du soleil (carte d'ombres, projection parallèle) : le même
objet y passe par une application affine de l'espace caméra. La direction que cette application envoie
sur l'axe de profondeur de la carte, sans bouger dans son image, est celle du soleil.

  python -I urti/tools/analyse.py capture.urti [--truth verite.txt]
"""

import argparse
import struct
import sys

import numpy as np


def read_capture(path):
    data = open(path, 'rb').read()
    h = struct.unpack_from('<12I', data, 0)
    assert h[0] == 0x49545255, 'pas une capture URTI'
    version, count, width, height, sw, sh = h[1], h[2], h[4], h[5], h[6], h[7]
    filled = h[8] | (h[9] << 32)
    off = 48
    slots = []
    if version >= 2:
        for _ in range(h[11]):
            s = struct.unpack_from('<6I', data, off)
            off += 24
            slots.append({'size': (s[0], s[1]), 'screen': s[2] == 2, 'used': s[2] != 0, 'bytes': s[3] | (s[4] << 32), 'layers': s[5]})
    draws = []
    for _ in range(count):
        r = struct.unpack_from('<10I2Q', data, off)
        off += 56
        draws.append({'slot': r[0], 'frame': r[1], 'kind': r[2], 'params': r[3:8], 'pipeline': r[8] | (r[9] << 32),
                      'start': r[10], 'end': r[11]})
    slot = h[3]
    if version >= 2:
        for s in slots:
            s['verts'] = np.frombuffer(data, dtype='<f4', count=s['bytes'] // 4, offset=off).reshape(-1, 4) if s['used'] else np.zeros((0, 4), np.float32)
            off += s['bytes'] if s['used'] else 0
        verts = slots[slot]['verts']
    else:
        verts = np.frombuffer(data, dtype='<f4', count=filled // 4, offset=off).reshape(-1, 4)
        off += filled
    depth = np.frombuffer(data, dtype='<f4', count=width * height, offset=off).reshape(height, width)
    all_draws = [d for d in draws if d['end'] > d['start']]
    draws = [d for d in all_draws if d['slot'] == slot]
    return {'draws': draws, 'all_draws': all_draws, 'slots': slots, 'main': slot, 'verts': verts, 'depth': depth,
            'screen': (sw, sh), 'size': (width, height), 'reversed': bool(h[10])}


def signature(d):
    return (d['kind'], d['pipeline'], d['params'])


def matched_pairs(cap, fa, fb):
    """Dessins de même signature et même nombre de sommets dans les frames fa et fb."""
    by = {}
    for d in cap['draws']:
        if d['frame'] in (fa, fb):
            by.setdefault((signature(d), d['frame']), []).append(d)
    pairs = []
    for (sig, frame), lst in by.items():
        if frame != fa:
            continue
        other = by.get((sig, fb), [])
        # signatures répétées (même maillage dessiné plusieurs fois) : appariées dans l'ordre d'émission ;
        # l'ajustement robuste écarte ensuite les erreurs et les objets qui bougent
        if len(lst) == len(other):
            for x, y in zip(lst, other):
                if x['end'] - x['start'] == y['end'] - y['start']:
                    pairs.append((x, y))
    return pairs


def to_view(clip, p11, aspect):
    w = clip[:, 3]
    return np.stack([clip[:, 0] / (p11 / aspect), clip[:, 1] / p11, w], 1)


def kabsch(a, b, weights=None):
    """Mouvement rigide b ≈ R a + t (moindres carrés, pondéré)."""
    w = np.ones(len(a)) if weights is None else weights
    w = w / w.sum()
    ca, cb = (a * w[:, None]).sum(0), (b * w[:, None]).sum(0)
    H = ((a - ca) * w[:, None]).T @ (b - cb)
    U, _, Vt = np.linalg.svd(H)
    D = np.diag([1, 1, np.sign(np.linalg.det(Vt.T @ U.T))])
    R = Vt.T @ D @ U.T
    return R, cb - R @ ca


def rigid_fit(c1, c2, p11, aspect, iterations=4):
    """Mouvement rigide robuste (les objets qui bougent eux-mêmes sont écartés) ; renvoie R, t, erreur."""
    a, b = to_view(c1, p11, aspect), to_view(c2, p11, aspect)
    weights = np.ones(len(a))
    for _ in range(iterations):
        R, t = kabsch(a, b, weights)
        r = np.linalg.norm(b - (a @ R.T + t), axis=1)
        scale = np.median(r) * 3 + 1e-9
        weights = (r < scale).astype(float) + 1e-6
    rel = np.median(r / np.maximum(np.linalg.norm(b, axis=1), 1e-6))
    return R, t, rel, weights > 0.5


def sun_direction(cap, frame, p11, aspect):
    """Direction du soleil (vers lui) dans l'espace caméra, à partir des cartes d'ombres de la frame."""
    results = []
    main = cap['main']
    for si, slot in enumerate(cap['slots']):
        if si == main or not slot['used'] or slot['screen']:
            continue
        # mêmes dessins (même maillage, mêmes paramètres) dans la vue et dans la carte, appariés dans l'ordre
        mine = [d for d in cap['all_draws'] if d['slot'] == main and d['frame'] == frame]
        theirs = [d for d in cap['all_draws'] if d['slot'] == si and d['frame'] == frame]
        groups = {}
        for d in mine:
            groups.setdefault((d['kind'], d['params']), [[], []])[0].append(d)
        for d in theirs:
            groups.setdefault((d['kind'], d['params']), [[], []])[1].append(d)
        A_all, B_all = [], []
        for (_, _), (a_list, b_list) in groups.items():
            if len(a_list) != len(b_list):
                continue
            for x, y in zip(a_list, b_list):
                if x['end'] - x['start'] != y['end'] - y['start']:
                    continue
                c = cap['verts'][x['start'] // 16:x['end'] // 16].astype(np.float64)
                s = slot['verts'][y['start'] // 16:y['end'] // 16].astype(np.float64)
                ok = c[:, 3] > 1e-3
                A_all.append(to_view(c[ok], p11, aspect))
                B_all.append(s[ok, :3] / s[ok, 3:4])
        if not A_all:
            continue
        V, S = np.concatenate(A_all), np.concatenate(B_all)
        X = np.concatenate([V, np.ones((len(V), 1))], 1)
        # application affine robuste (des dessins mal appariés ou animés sont écartés)
        keep = np.ones(len(V), bool)
        for _ in range(4):
            M, *_ = np.linalg.lstsq(X[keep], S[keep], rcond=None)
            r = np.linalg.norm(X @ M - S, axis=1)
            keep = r < np.median(r) * 3 + 1e-9
        A = M[:3, :].T  # S ≈ A V + b
        d = np.linalg.solve(A, np.array([0.0, 0.0, 1.0]))  # le long de la profondeur de la carte : vers la scène
        toward_sun = -d / np.linalg.norm(d)
        results.append((si, toward_sun, len(V), float(np.median(r)), keep.mean()))
    return results


def view_rotation(eye, at):
    z = np.array(at) - np.array(eye)
    z /= np.linalg.norm(z)
    x = np.array([z[2], 0.0, -z[0]])
    x /= np.linalg.norm(x)
    y = np.cross(z, x)
    return np.stack([x, y, z])  # lignes : axes de la caméra dans le monde


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('capture')
    ap.add_argument('--truth')
    a = ap.parse_args()
    cap = read_capture(a.capture)
    sw, sh = cap['screen']
    width, height = cap['size']
    aspect = width / height
    frames = sorted({d['frame'] for d in cap['draws']})
    print(f'{len(cap["draws"])} dessins sur la profondeur principale, frames {frames}, {len(cap["verts"])} sommets')
    if len(frames) < 2:
        print('une seule frame : pas d\'auto-étalonnage possible')
        return
    fa, fb = frames[0], frames[-1]
    pairs = matched_pairs(cap, fa, fb)
    print(f'{len(pairs)} dessins retrouvés entre les frames {fa} et {fb}')
    c1 = np.concatenate([cap['verts'][p[0]['start'] // 16:p[0]['end'] // 16] for p in pairs])
    c2 = np.concatenate([cap['verts'][p[1]['start'] // 16:p[1]['end'] // 16] for p in pairs])
    ok = (c1[:, 3] > 1e-3) & (c2[:, 3] > 1e-3)
    c1, c2 = c1[ok].astype(np.float64), c2[ok].astype(np.float64)

    # champ de vision : recherche du plus rigide, grossière puis fine
    best = None
    for fov in np.radians(np.linspace(20, 120, 101)):
        p11 = 1 / np.tan(fov / 2)
        err = rigid_fit(c1, c2, p11, aspect)[2]
        if best is None or err < best[1]:
            best = (fov, err)
    for fov in np.linspace(best[0] - np.radians(1.5), best[0] + np.radians(1.5), 61):
        p11 = 1 / np.tan(fov / 2)
        err = rigid_fit(c1, c2, p11, aspect)[2]
        if err < best[1]:
            best = (fov, err)
    fov, err = best
    p11 = 1 / np.tan(fov / 2)
    R, t, err, inliers = rigid_fit(c1, c2, p11, aspect)
    angle = np.degrees(np.arccos(np.clip((np.trace(R) - 1) / 2, -1, 1)))
    print(f'champ de vision vertical estimé : {np.degrees(fov):.2f}° ; erreur de rigidité {100 * err:.3f} % ; '
          f'{100 * inliers.mean():.1f} % des sommets immobiles')
    print(f'mouvement de la caméra : rotation {angle:.3f}°, translation {np.linalg.norm(t):.3f} (unités du jeu)')
    suns = sun_direction(cap, fb, p11, aspect)
    for si, sun, n, res, kept in suns:
        print(f'soleil (carte d\'ombres du flux {si}, {cap["slots"][si]["size"][0]}x{cap["slots"][si]["size"][1]}) : direction vers le soleil '
              f'dans l\'espace caméra {np.round(sun, 4)} ; {n} sommets, {100 * kept:.0f} % gardés')
    if not suns:
        print('soleil : aucune carte d\'ombres appariée')

    if a.truth:
        truth = {}
        for line in open(a.truth):
            v = line.split()
            truth[int(v[0])] = (np.array(list(map(float, v[1:4]))), np.array(list(map(float, v[4:20]))).reshape(4, 4),
                                np.array(list(map(float, v[20:23]))) if len(v) >= 23 else None)
        # le banc : fov vertical 1,05 rad ; frame de capture = numéro de frame de l'application
        print(f'vérité : champ de vision 60.16° ; écart {np.degrees(fov) - 60.16:+.2f}°')
        if fa in truth and fb in truth:
            ea, eb = truth[fa][0], truth[fb][0]
            print(f'vérité : la caméra s\'est déplacée de {np.linalg.norm(eb - ea):.3f} ; estimé {np.linalg.norm(t):.3f}')
            if truth[fb][2] is not None and suns:
                true_sun = view_rotation(eb, [0.0, 0.5, 1.0]) @ truth[fb][2]
                for si, sun, *_ in suns:
                    err_deg = np.degrees(np.arccos(np.clip(np.dot(sun, true_sun), -1, 1)))
                    print(f'vérité : soleil dans l\'espace caméra {np.round(true_sun, 4)} ; écart {err_deg:.2f}°')


if __name__ == '__main__':
    sys.exit(main())
