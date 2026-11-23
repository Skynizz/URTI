"""URTI sandbox: do the shadows URTI adds look like the game's?

    python urti_sandbox_shadows.py <RT folder> <full folder> <partial folder> <output folder>

Three identical launches (same frames):
  - RT: the "game's" shadow map forgets objects (URTI_SANDBOX_GAMESHADOW=partial), URTI shadows them;
  - full: native, the game shadows everything: the reference, what URTI should give;
  - partial: native with the forgotten shadows: what you would get without URTI.
Measured area: pixels the full shadow darkens and the partial one leaves lit (the shadows
URTI must add). Measurements:
  - recovered: share of the partial / full difference that URTI fills (100 %: shadows identical to the game's);
  - darkness: mean light of the area with URTI / with the game, per channel (1: the same; > 1: too
    bright; < 1: too dark); a difference between channels is a wrong tint;
  - elsewhere: mean difference RT / full outside the area (extra RT shadows, edges);
  - flicker: frame-to-frame change that is not in the reference, inside the area.
Output: shadows.png (full | partial | RT | amplified difference) for the first frame, measurements.txt.
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


def frames(folder):
    out = {}
    for name in os.listdir(folder):
        m = re.match(r'urti_photo_(\d+)\.ppm$', name)
        if m:
            out[int(m.group(1))] = os.path.join(folder, name)
    return out


def main():
    rt_dir, full_dir, part_dir, out_dir = sys.argv[1:5]
    os.makedirs(out_dir, exist_ok=True)
    rt, full, part = frames(rt_dir), frames(full_dir), frames(part_dir)
    keys = sorted(set(rt) & set(full) & set(part))
    if not keys:
        sys.exit('no common frame')
    lum = np.array([0.2126, 0.7152, 0.0722], np.float32)
    gap_rt = gap_none = 0.0
    zone_pixels = 0
    sum_rt = np.zeros(3)
    sum_full = np.zeros(3)
    sum_part = np.zeros(3)
    elsewhere = elsewhere_n = 0.0
    flicker = flicker_n = 0.0
    prev = None
    for i, k in enumerate(keys):
        a, b, c = load(rt[k]), load(full[k]), load(part[k])
        # light (snapshot in gamma 2.2)
        la, lb, lc = a ** 2.2, b ** 2.2, c ** 2.2
        yb, yc = lb @ lum, lc @ lum
        zone = (yc - yb > 0.02) & (yc > 0.05)
        n = int(zone.sum())
        if n:
            gap_rt += float(np.abs(la - lb)[zone].sum())
            gap_none += float(np.abs(lc - lb)[zone].sum())
            sum_rt += la[zone].sum(axis=0)
            sum_full += lb[zone].sum(axis=0)
            sum_part += lc[zone].sum(axis=0)
            zone_pixels += n
        other = ~zone
        elsewhere += float(np.abs(a - b)[other].mean())
        elsewhere_n += 1
        if prev is not None:
            pa, pb, pzone = prev
            both = zone & pzone
            if both.any():
                flicker += float(np.abs((a - pa) - (b - pb))[both].mean())
                flicker_n += 1
        prev = (a, b, zone)
        if i == 0:
            diff = np.clip(np.abs(a - b) * 4.0, 0, 1)
            row = np.concatenate([b, c, a, diff], axis=1)
            Image.fromarray((row * 255).astype(np.uint8)).save(os.path.join(out_dir, 'shadows.png'))
    lines = [f'frames compared: {len(keys)} ({keys[0]} to {keys[-1]})']
    if zone_pixels:
        mean_full, mean_rt, mean_part = sum_full / zone_pixels, sum_rt / zone_pixels, sum_part / zone_pixels
        ratio = mean_rt / np.maximum(mean_full, 1e-6)
        lines.append(f'area of the shadows to add: {zone_pixels / len(keys):.0f} pixels per frame')
        lines.append(f'recovered (difference without URTI filled): {100.0 * (1.0 - gap_rt / max(gap_none, 1e-9)):.1f} %')
        lines.append('darkness (light RT / game, red green blue): ' + ' '.join(f'{v:.3f}' for v in ratio))
        lines.append('without URTI (partial / game): ' + ' '.join(f'{v:.3f}' for v in mean_part / np.maximum(mean_full, 1e-6)))
    else:
        lines.append('area of the shadows to add: empty')
    lines.append(f'elsewhere (mean difference RT / game outside the area): {100.0 * elsewhere / max(elsewhere_n, 1):.3f} %')
    lines.append(f'flicker added inside the area: {100.0 * flicker / max(flicker_n, 1):.3f} %')
    text = '\n'.join(lines)
    print(text)
    with open(os.path.join(out_dir, 'measurements.txt'), 'w', encoding='utf-8') as f:
        f.write(text + '\n')


if __name__ == '__main__':
    main()
