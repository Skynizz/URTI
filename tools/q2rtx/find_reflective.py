"""Repère dans les cartes de pak0.pak les surfaces qui reflètent (eau, verre : SURF_WARP, SURF_TRANS*)
et imprime les plus grandes zones (centre, aire, normale), pour y placer la caméra.

    python -I tools/q2rtx/find_reflective.py F:/Q2RTX-KRC/baseq2/pak0.pak
"""
import struct
import sys
from collections import defaultdict

SURF_WARP, SURF_TRANS33, SURF_TRANS66 = 0x8, 0x10, 0x20


def pak_files(path):
    f = open(path, 'rb')
    _, off, ln = struct.unpack('<4sii', f.read(12))
    f.seek(off)
    out = {}
    for _ in range(ln // 64):
        n, o, s = struct.unpack('<56sii', f.read(64))
        out[n.split(b'\0')[0].decode()] = (o, s)
    return f, out


def lump(data, i):
    off, ln = struct.unpack_from('<ii', data, 8 + 8 * i)
    return data[off:off + ln]


def analyse(data):
    verts = [struct.unpack_from('<3f', lump(data, 2), i) for i in range(0, len(lump(data, 2)), 12)]
    texinfo = []
    ti = lump(data, 5)
    for i in range(0, len(ti), 76):
        flags = struct.unpack_from('<i', ti, i + 32)[0]
        name = ti[i + 40:i + 72].split(b'\0')[0].decode(errors='replace')
        texinfo.append((flags, name))
    edges = [struct.unpack_from('<2H', lump(data, 11), i) for i in range(0, len(lump(data, 11)), 4)]
    surfedges = [struct.unpack_from('<i', lump(data, 12), i)[0] for i in range(0, len(lump(data, 12)), 4)]
    planes = [struct.unpack_from('<3f', lump(data, 1), i) for i in range(0, len(lump(data, 1)), 20)]
    faces = lump(data, 6)
    groups = defaultdict(lambda: [0.0, [0.0, 0.0, 0.0], 0, None])
    for i in range(0, len(faces), 20):
        plane, side, first, num, tex = struct.unpack_from('<HhiHh', faces, i)
        flags, name = texinfo[tex]
        kind = 'eau' if flags & SURF_WARP else ('verre' if flags & (SURF_TRANS33 | SURF_TRANS66) else None)
        if kind is None:
            continue
        pts = []
        for k in range(num):
            se = surfedges[first + k]
            pts.append(verts[edges[se][0]] if se >= 0 else verts[edges[-se][1]])
        # aire du polygone
        area = 0.0
        for k in range(1, num - 1):
            a = [pts[k][j] - pts[0][j] for j in range(3)]
            b = [pts[k + 1][j] - pts[0][j] for j in range(3)]
            c = (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])
            area += 0.5 * (c[0] ** 2 + c[1] ** 2 + c[2] ** 2) ** 0.5
        cx = [sum(p[j] for p in pts) / num for j in range(3)]
        key = (kind, name, tuple(int(cx[j] // 256) for j in range(3)))
        g = groups[key]
        g[0] += area
        for j in range(3):
            g[1][j] += cx[j] * area
        g[2] += 1
        n = planes[plane]
        g[3] = tuple(-x for x in n) if side else n
    res = []
    for (kind, name, _), (area, c, count, n) in groups.items():
        res.append((area, kind, name, [c[j] / area for j in range(3)] if area > 0 else c, n, count))
    res.sort(reverse=True)
    return res


def main():
    f, files = pak_files(sys.argv[1])
    for m in ['maps/demo1.bsp', 'maps/demo2.bsp', 'maps/demo3.bsp']:
        o, s = files[m]
        f.seek(o)
        data = f.read(s)
        print(f'== {m}')
        for area, kind, name, c, n, count in analyse(data)[:8]:
            print(f'  {kind:5s} {name:24s} aire {area:9.0f}  centre ({c[0]:.0f}, {c[1]:.0f}, {c[2]:.0f})  '
                  f'normale ({n[0]:.2f}, {n[1]:.2f}, {n[2]:.2f})  {count} faces')


if __name__ == '__main__':
    main()
