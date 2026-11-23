"""URTI: analysis of a burst taken in game (F8 key, or trigger file urti_burst
in the game folder).

    python urti_burst.py <urti_burst_N folder> <output folder> [urti_log.txt log]

The burst holds, for 30 consecutive frames, the game's image without URTI (before_N.ppm) and the same image with
URTI (after_N.ppm). URTI's contribution is C = after - before; what jumps is the change of C from one
frame to the next, independent of what the scene does (wind, character, slow camera). Measurements:
  - effect: share of pixels URTI changes by more than 2 % (shadows: it darkens; reflections: the rest);
  - shake / jump: share of pixels whose C changes by more than 3 % / 8 % from one frame to the next, overall
    and in each area (shadows, reflections);
  - scene motion: mean change of the image without URTI (still camera: small);
  - per frame: jumping pixels, objects entering / leaving the RT scene (BURST lines of the log).
Output: map.png (image with URTI darkened, jumping pixels in red, the redder the more often),
area.png (the 96 × 96 pixel area that jumps the most, over six consecutive frames: without URTI on top, with it
at the bottom), measurements.txt.
"""
import os
import re
import sys

import numpy as np
from PIL import Image


def load(path):
    with open(path, 'rb') as f:
        data = f.read()
    m = re.match(rb'P6\s+(\d+)\s+(\d+)\s+(\d+)\s', data)
    w, h = int(m.group(1)), int(m.group(2))
    return np.frombuffer(data[m.end():m.end() + w * h * 3], np.uint8).reshape(h, w, 3).astype(np.float32) / 255.0


def main():
    folder, out = sys.argv[1], sys.argv[2]
    log = sys.argv[3] if len(sys.argv) > 3 else None
    os.makedirs(out, exist_ok=True)
    idx = sorted(int(m.group(1)) for n in os.listdir(folder) if (m := re.match(r'after_(\d+)\.ppm$', n)))
    idx = [i for i in idx if os.path.exists(os.path.join(folder, f'before_{i}.ppm'))]
    if len(idx) < 2:
        sys.exit('incomplete burst')
    before = [load(os.path.join(folder, f'before_{i}.ppm')) for i in idx]
    after = [load(os.path.join(folder, f'after_{i}.ppm')) for i in idx]
    contrib = [a - b for a, b in zip(after, before)]
    mean_c = np.mean(contrib, axis=0).mean(axis=2)
    strength = np.mean([np.abs(c).max(axis=2) for c in contrib], axis=0)
    shadow_zone = (mean_c < -0.01) & (strength > 0.02)
    refl_zone = (strength > 0.02) & ~shadow_zone
    h, w = mean_c.shape
    jumps = np.zeros((h, w), np.float32)
    lines = []
    rows = []
    trem_all = jump_all = 0.0
    trem_s = jump_s = trem_r = jump_r = 0.0
    scene = 0.0
    for k in range(1, len(idx)):
        d = np.abs(contrib[k] - contrib[k - 1]).max(axis=2)
        scene += float(np.abs(before[k] - before[k - 1]).mean())
        t, j = d > 0.03, d > 0.08
        jumps += j
        trem_all += t.mean()
        jump_all += j.mean()
        if shadow_zone.any():
            trem_s += t[shadow_zone].mean()
            jump_s += j[shadow_zone].mean()
        if refl_zone.any():
            trem_r += t[refl_zone].mean()
            jump_r += j[refl_zone].mean()
        rows.append((idx[k], 100.0 * j.mean(), 100.0 * (j[refl_zone].mean() if refl_zone.any() else 0.0),
                     100.0 * (j[shadow_zone].mean() if shadow_zone.any() else 0.0)))
    n = len(idx) - 1
    churn = {}
    burst = re.search(r'(\d+)$', os.path.normpath(folder))
    if log and os.path.exists(log) and burst:
        with open(log, encoding='utf-8', errors='replace') as f:
            for line in f:
                m = re.match(r'BURST (\d+) (\d+) frame \d+ RT objects (\d+) in (\d+) out (\d+)', line)
                if m and m.group(1) == burst.group(1):
                    churn[int(m.group(2))] = (int(m.group(3)), int(m.group(4)), int(m.group(5)))
    lines.append(f'images : {len(idx)} ({w}x{h})')
    lines.append(f'scene motion (without URTI): {100.0 * scene / n:.2f} % per frame')
    lines.append(f'URTI effect: {100.0 * (strength > 0.02).mean():.1f} % of pixels changed by more than 2 % '
                 f'(shadows {100.0 * shadow_zone.mean():.1f} %, reflections {100.0 * refl_zone.mean():.1f} %)')
    lines.append(f'shake (> 3 %): {100.0 * trem_all / n:.2f} % of pixels; shadows {100.0 * trem_s / n:.2f} %, reflections {100.0 * trem_r / n:.2f} % (of their area)')
    lines.append(f'jump (> 8 %): {100.0 * jump_all / n:.3f} % of pixels; shadows {100.0 * jump_s / n:.2f} %, reflections {100.0 * jump_r / n:.2f} % (of their area)')
    lines.append('per frame: no., jump % (all, reflections, shadows), RT objects, in, out')
    for i, a, r, s in rows:
        c = churn.get(i)
        lines.append(f'  {i:3d}  {a:6.3f} {r:6.2f} {s:6.2f}' + (f'   {c[0]:4d} {c[1]:4d} {c[2]:4d}' if c else ''))
    text = '\n'.join(lines)
    print(text)
    with open(os.path.join(out, 'measurements.txt'), 'w', encoding='utf-8') as f:
        f.write(text + '\n')
    # map: image with URTI darkened, jumps in red
    base = after[-1] * 0.45
    heat = np.clip(jumps / max(1.0, n * 0.25), 0, 1)
    base[..., 0] = np.maximum(base[..., 0], heat)
    Image.fromarray((np.clip(base, 0, 1) * 255).astype(np.uint8)).save(os.path.join(out, 'map.png'))
    # area that jumps the most: six consecutive frames, without URTI on top, with it below
    s = 96
    if h > s and w > s:
        acc = jumps.cumsum(0).cumsum(1)
        best, by, bx = -1.0, 0, 0
        for y in range(0, h - s, 16):
            for x in range(0, w - s, 16):
                v = acc[y + s, x + s] - acc[y, x + s] - acc[y + s, x] + acc[y, x]
                if v > best:
                    best, by, bx = v, y, x
        k0 = max(0, int(np.argmax([r[1] for r in rows])) - 2)
        frames = list(range(k0, min(len(idx), k0 + 6)))
        strip = np.zeros((2 * s, len(frames) * s, 3), np.float32)
        for c, k in enumerate(frames):
            strip[:s, c * s:(c + 1) * s] = before[k][by:by + s, bx:bx + s]
            strip[s:, c * s:(c + 1) * s] = after[k][by:by + s, bx:bx + s]
        Image.fromarray((strip * 255).astype(np.uint8)).resize((len(frames) * s * 3, 2 * s * 3), Image.NEAREST).save(os.path.join(out, 'area.png'))
        with open(os.path.join(out, 'measurements.txt'), 'a', encoding='utf-8') as f:
            f.write(f'area that jumps the most: x {bx}, y {by} (frames {idx[frames[0]]} to {idx[frames[-1]]})\n')


if __name__ == '__main__':
    main()
