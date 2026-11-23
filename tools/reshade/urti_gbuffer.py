"""URTI: study of a game's G-buffer (pass survey, urti_passes files: urti_snapshot_NN_pass_NNN.raw)

    python urti_gbuffer.py <game folder or folder of the .raw files> [output]

Each .raw (raw texels at half resolution, every channel) is decoded channel by channel: one sheet per
image (one channel = one grey thumbnail, raw value 0..1, floats brought back by their 99.5th percentile) and one
statistics line per channel (min, max, mean, number of distinct values, share at 0 and at 1). The
log (urti_log.txt) tells which passes are the G-buffer targets.
"""
import os
import re
import sys

import numpy as np
from PIL import Image, ImageDraw

# DXGI format -> (numpy type, number of channels, normalisation); None: special decoding
FORMATS = {
    2: ('<f4', 4, None), 16: ('<f4', 2, None), 41: ('<f4', 1, None),
    9: ('<f2', 4, None), 10: ('<f2', 4, None), 11: ('<u2', 4, 65535), 33: ('<f2', 2, None), 34: ('<f2', 2, None),
    35: ('<u2', 2, 65535), 53: ('<f2', 1, None), 54: ('<f2', 1, None), 56: ('<u2', 1, 65535),
    27: ('u1', 4, 255), 28: ('u1', 4, 255), 29: ('u1', 4, 255), 30: ('u1', 4, None), 31: ('i1', 4, 127),
    87: ('u1', 4, 255), 90: ('u1', 4, 255), 91: ('u1', 4, 255),
    48: ('u1', 2, 255), 49: ('u1', 2, 255), 60: ('u1', 1, 255), 61: ('u1', 1, 255),
}
NAMES = {2: 'RGBA32F', 9: 'RGBA16', 10: 'RGBA16F', 11: 'RGBA16_UNORM', 16: 'RG32F', 23: 'RGB10A2', 24: 'RGB10A2_UNORM',
         25: 'RGB10A2_UINT', 26: 'R11G11B10F', 27: 'RGBA8', 28: 'RGBA8_UNORM', 29: 'RGBA8_SRGB', 30: 'RGBA8_UINT',
         31: 'RGBA8_SNORM', 33: 'RG16', 34: 'RG16F', 35: 'RG16_UNORM', 41: 'R32F', 53: 'R16', 54: 'R16F',
         56: 'R16_UNORM', 48: 'RG8', 49: 'RG8_UNORM', 60: 'R8', 61: 'R8_UNORM', 87: 'BGRA8', 90: 'BGRA8', 91: 'BGRA8_SRGB'}


def decode(path):
    with open(path, 'rb') as f:
        data = f.read()
    m = re.match(rb'URTIRAW (\d+) (\d+) (\d+) (\d+)\n', data)
    fmt, w, h, texel = (int(g) for g in m.groups())
    body = data[m.end():m.end() + w * h * texel]
    if fmt in (23, 24, 25):
        u = np.frombuffer(body, '<u4').reshape(h, w)
        ch = [((u >> s) & 1023).astype(np.float32) / 1023 for s in (0, 10, 20)] + [(u >> 30).astype(np.float32) / 3]
        return fmt, np.stack(ch, -1)
    if fmt == 26:
        u = np.frombuffer(body, '<u4').reshape(h, w)

        def small(bits, mb):
            m_, e = (bits & ((1 << mb) - 1)).astype(np.float32), (bits >> mb).astype(np.int32)
            return np.where(e > 0, np.ldexp(1 + m_ / (1 << mb), e - 15), np.ldexp(m_ / (1 << mb), -14)).astype(np.float32)
        return fmt, np.stack([small(u & 2047, 6), small((u >> 11) & 2047, 6), small(u >> 22, 5)], -1)
    if fmt not in FORMATS:
        return fmt, None
    dt, n, norm = FORMATS[fmt]
    a = np.frombuffer(body, dt).reshape(h, w, n).astype(np.float32)
    if norm:
        a /= norm
    if fmt in (87, 90, 91):
        a = a[..., [2, 1, 0, 3]]
    return fmt, a


def show(c):
    """channel -> 8-bit grey: 0..1 as is, otherwise brought back by the 99.5th percentile of its absolute value"""
    c = np.nan_to_num(c, nan=0, posinf=0, neginf=0)
    lo = float(c.min())
    if lo >= 0 and float(c.max()) <= 1.0:
        v = c
    elif lo < 0:
        p = float(np.percentile(np.abs(c), 99.5)) or 1.0
        v = 0.5 + 0.5 * c / p
    else:
        v = c / (float(np.percentile(c, 99.5)) or 1.0)
    return (np.clip(v, 0, 1) * 255).astype(np.uint8)


def main():
    src = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(src, 'urti_gbuffer')
    os.makedirs(out, exist_ok=True)
    gb = {}
    log = os.path.join(src, 'urti_log.txt')
    if os.path.exists(log):
        for line in open(log, encoding='utf-8', errors='replace'):
            if (m := re.search(r'G-buffer target (\d+) = pass (\d+)', line)):
                gb[int(m.group(2))] = int(m.group(1))
    files = sorted(n for n in os.listdir(src) if re.match(r'urti_(snapshot|pass)_.*\.raw$', n))
    for name in files:
        fmt, a = decode(os.path.join(src, name))
        order = int(re.search(r'pass_(\d+)', name).group(1))
        tag = f'  [G-buffer target {gb[order]}]' if order in gb else ''
        if a is None:
            print(f'{name}: format {fmt} not decoded')
            continue
        h, w, n = a.shape
        print(f'{name}: {NAMES.get(fmt, fmt)} {w}x{h}{tag}')
        tiles = []
        for k in range(n):
            c = a[..., k]
            fin = c[np.isfinite(c)]
            q = np.round(np.clip(fin, -4, 4) * 255).astype(np.int32)
            print(f'   channel {k}: min {fin.min():8.3f} max {fin.max():8.3f} mean {fin.mean():7.3f} '
                  f'distinct {len(np.unique(q)):4d}  at 0 {100 * np.mean(fin == 0):5.1f} %  at 1 {100 * np.mean(fin == 1):5.1f} %')
            img = Image.fromarray(show(c)).resize((w // 2, h // 2), Image.BILINEAR)
            ImageDraw.Draw(img).text((6, 4), f'{order:03d} c{k}', fill=255)
            tiles.append(img)
        sheet = Image.new('L', (sum(t.width for t in tiles), tiles[0].height))
        x = 0
        for t in tiles:
            sheet.paste(t, (x, 0))
            x += t.width
        sheet.save(os.path.join(out, name[:-4] + '.png'))
    print(f'sheets: {out}')


if __name__ == '__main__':
    main()
