"""URTI: offline analysis of a capture (.urti): objects found from one frame to the next, field of
view and camera motion estimated without knowing anything about the game (self-calibration), compared with the
ground truth if it exists (bench: URTI_TRUTH).

Principle. In clip space, w is the depth along the view axis. With the vertical field of
view (P11 = 1 / tan(fov / 2), P00 = P11 × height / width), each vertex becomes a point of
camera space. A still object seen in two frames must then move by a single rigid motion
(rotation + translation): we look for the field of view for which that motion is the most rigid.

Sun. The game also draws the scene seen from the sun (shadow map, parallel projection): the same
object goes there through an affine map of camera space. The direction that map sends
onto the map's depth axis, without moving in its image, is the sun's.

  python -I urti/tools/analyse.py capture.urti [--truth truth.txt]
"""

import argparse
import struct
import sys

import numpy as np


def read_capture(path):
    data = open(path, 'rb').read()
    h = struct.unpack_from('<12I', data, 0)
    assert h[0] == 0x49545255, 'not a URTI capture'
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
    """Draws with the same signature and the same vertex count in frames fa and fb."""
    by = {}
    for d in cap['draws']:
        if d['frame'] in (fa, fb):
            by.setdefault((signature(d), d['frame']), []).append(d)
    pairs = []
    for (sig, frame), lst in by.items():
        if frame != fa:
            continue
        other = by.get((sig, fb), [])
        # repeated signatures (the same mesh drawn several times): paired in emission order;
        # the robust fit then discards errors and moving objects
        if len(lst) == len(other):
            for x, y in zip(lst, other):
                if x['end'] - x['start'] == y['end'] - y['start']:
                    pairs.append((x, y))
    return pairs


def to_view(clip, p11, aspect):
    w = clip[:, 3]
    return np.stack([clip[:, 0] / (p11 / aspect), clip[:, 1] / p11, w], 1)


def kabsch(a, b, weights=None):
    """Rigid motion b ≈ R a + t (weighted least squares)."""
    w = np.ones(len(a)) if weights is None else weights
    w = w / w.sum()
    ca, cb = (a * w[:, None]).sum(0), (b * w[:, None]).sum(0)
    H = ((a - ca) * w[:, None]).T @ (b - cb)
    U, _, Vt = np.linalg.svd(H)
    D = np.diag([1, 1, np.sign(np.linalg.det(Vt.T @ U.T))])
    R = Vt.T @ D @ U.T
    return R, cb - R @ ca


def rigid_fit(c1, c2, p11, aspect, iterations=4):
    """Robust rigid motion (objects that move on their own are discarded); returns R, t, error."""
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
    """Sun direction (toward it) in camera space, from the frame's shadow maps.

    The same mesh may appear several times in a map (cascades of an atlas, each with its own
    projection): an affine map is fitted per pair "view draw / map draw",
    each gives a direction, and the direction on which the most vertices agree is kept.
    """
    results = []
    main = cap['main']
    mine = [d for d in cap['all_draws'] if d['slot'] == main and d['frame'] == frame]
    for si, slot in enumerate(cap['slots']):
        if si == main or not slot['used'] or slot['screen']:
            continue
        theirs = {}
        for d in cap['all_draws']:
            if d['slot'] == si and d['frame'] == frame:
                theirs.setdefault((d['kind'], d['params'], d['end'] - d['start']), []).append(d)
        dirs, weights = [], []
        rank = {}
        for x in mine:
            kx = (x['kind'], x['params'], x['end'] - x['start'])
            my_rank = rank.get(kx, 0)
            rank[kx] = my_rank + 1
            for m, y in enumerate(theirs.get(kx, [])[:64]):
                c = cap['verts'][x['start'] // 16:x['end'] // 16].astype(np.float64)
                sv = slot['verts'][y['start'] // 16:y['end'] // 16].astype(np.float64)
                ok = (c[:, 3] > 1e-3) & (np.abs(sv[:, 3]) > 1e-6)
                if ok.sum() < 24:
                    continue
                V = to_view(c[ok], p11, aspect)
                S = sv[ok, :3] / sv[ok, 3:4]
                X = np.concatenate([V, np.ones((len(V), 1))], 1)
                M, *_ = np.linalg.lstsq(X, S, rcond=None)
                r = np.linalg.norm(X @ M - S, axis=1)
                scale = np.abs(S - S.mean(0)).max() + 1e-12
                if np.median(r) > 1e-3 * scale or np.linalg.cond(X) > 1e8:
                    continue  # not a parallel projection of this mesh (another object, or animated)
                A = M[:3, :].T
                try:
                    d = np.linalg.solve(A, np.array([0.0, 0.0, 1.0]))
                except np.linalg.LinAlgError:
                    continue
                dirs.append(-d / np.linalg.norm(d))
                weights.append(ok.sum() * (8 if m == my_rank else 1))  # same rank: probably the same object
        if not dirs:
            continue
        dirs, weights = np.array(dirs), np.array(weights, float)
        # direction (up to sign) on which the most vertices agree, within 2°
        cos = np.abs(dirs @ dirs.T)
        votes = (cos > np.cos(np.radians(2))) @ weights
        best = int(np.argmax(votes))
        agree = np.abs(dirs @ dirs[best]) > np.cos(np.radians(2))
        aligned = dirs[agree] * np.sign(dirs[agree] @ dirs[best])[:, None]
        sun = aligned.T @ weights[agree]
        sun /= np.linalg.norm(sun)
        results.append((si, sun, int(weights[agree].sum()), len(dirs), agree.sum() / len(dirs)))
    return results


def view_rotation(eye, at):
    z = np.array(at) - np.array(eye)
    z /= np.linalg.norm(z)
    x = np.array([z[2], 0.0, -z[0]])
    x /= np.linalg.norm(x)
    y = np.cross(z, x)
    return np.stack([x, y, z])  # rows: the camera's axes in the world


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('capture')
    ap.add_argument('--truth')
    ap.add_argument('--fov', type=float, help='imposed field of view (degrees), without self-calibration')
    a = ap.parse_args()
    cap = read_capture(a.capture)
    sw, sh = cap['screen']
    width, height = cap['size']
    aspect = width / height
    frames = sorted({d['frame'] for d in cap['draws']})
    print(f'{len(cap["draws"])} draws on the main depth, frames {frames}, {len(cap["verts"])} vertices')
    if len(frames) < 2:
        print('a single frame: no self-calibration possible')
        return
    fa, fb = frames[0], frames[-1]
    pairs = matched_pairs(cap, fa, fb)
    print(f'{len(pairs)} draws found between frames {fa} and {fb}')
    c1 = np.concatenate([cap['verts'][p[0]['start'] // 16:p[0]['end'] // 16] for p in pairs])
    c2 = np.concatenate([cap['verts'][p[1]['start'] // 16:p[1]['end'] // 16] for p in pairs])
    ok = (c1[:, 3] > 1e-3) & (c2[:, 3] > 1e-3)
    c1, c2 = c1[ok].astype(np.float64), c2[ok].astype(np.float64)

    # field of view: search for the most rigid, coarse then fine
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
    if a.fov:
        fov = np.radians(a.fov)
    p11 = 1 / np.tan(fov / 2)
    R, t, err, inliers = rigid_fit(c1, c2, p11, aspect)
    angle = np.degrees(np.arccos(np.clip((np.trace(R) - 1) / 2, -1, 1)))
    print(f'estimated vertical field of view: {np.degrees(fov):.2f}°; rigidity error {100 * err:.3f} %; '
          f'{100 * inliers.mean():.1f} % of vertices still')
    print(f'camera motion: rotation {angle:.3f}°, translation {np.linalg.norm(t):.3f} (game units)')
    suns = sun_direction(cap, fb, p11, aspect)
    for si, sun, n, pairs, agree in suns:
        print(f'sun (shadow map of stream {si}, {cap["slots"][si]["size"][0]}x{cap["slots"][si]["size"][1]}): direction '
              f'in camera space {np.round(sun, 4)} (up to sign); {pairs} pairs, {100 * agree:.0f} % agreement, {n} vertices')
    if not suns:
        print('sun: no shadow map paired')

    if a.truth:
        truth = {}
        for line in open(a.truth):
            v = line.split()
            truth[int(v[0])] = (np.array(list(map(float, v[1:4]))), np.array(list(map(float, v[4:20]))).reshape(4, 4),
                                np.array(list(map(float, v[20:23]))) if len(v) >= 23 else None)
        # the bench: vertical fov 1.05 rad; capture frame = the application's frame number
        print(f'truth: field of view 60.16°; difference {np.degrees(fov) - 60.16:+.2f}°')
        if fa in truth and fb in truth:
            ea, eb = truth[fa][0], truth[fb][0]
            print(f'truth: the camera moved by {np.linalg.norm(eb - ea):.3f}; estimated {np.linalg.norm(t):.3f}')
            if truth[fb][2] is not None and suns:
                true_sun = view_rotation(eb, [0.0, 0.5, 1.0]) @ truth[fb][2]
                for si, sun, *_ in suns:
                    err_deg = np.degrees(np.arccos(np.clip(np.dot(sun, true_sun), -1, 1)))
                    print(f'truth: sun in camera space {np.round(true_sun, 4)}; difference {err_deg:.2f}°')


if __name__ == '__main__':
    sys.exit(main())
