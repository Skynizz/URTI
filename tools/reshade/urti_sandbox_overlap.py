"""Analysis for urti_sandbox_overlap.sh: "jump" events per pixel and per frame.

A jump at (pixel, t): the image's change between t-1 and t departs by more than 8 % from the
reference's at the same pixel, at the same moment. References: ground truth for launches with RT
reflections, native for shadows alone (without RT reflections, the ground truth would have missing reflections).
Events are counted (not percentages that get subtracted: events overlap):
  - shadows alone S, reflections alone R, both B;
  - B explained by S or R, and B that appears in neither S nor R: due to their overlap;
  - floor: native against ground truth on pixels without RT reflection (what already jumps without URTI).
Output: overlap.png (red: jump only when both are on; green: explained by the
shadows; blue: by the reflections), measurements.txt.
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


def series(folder):
    frames = sorted(int(m.group(1)) for m in (re.match(r'urti_photo_(\d+)\.ppm$', n) for n in os.listdir(folder)) if m)
    return frames, [load(os.path.join(folder, f'urti_photo_{f}.ppm')) for f in frames]


def events(run, ref):
    # (frames - 1, h, w): a jump at each moment
    return np.stack([np.abs((run[i] - run[i - 1]) - (ref[i] - ref[i - 1])).mean(axis=2) > 0.08 for i in range(1, len(run))])


def main():
    out = sys.argv[1]
    names = ['both', 'reflections', 'shadows', 'truth', 'native']
    data = {}
    for n in names:
        frames, ims = series(os.path.join(out, n))
        data[n] = (frames, ims)
    common = set(data['both'][0])
    for n in names:
        common &= set(data[n][0])
    if len(common) < 3:
        print('not enough common snapshots')
        return 1
    pick = lambda n: [im for f, im in zip(*data[n]) if f in common]
    D = events(pick('both'), pick('truth'))
    R = events(pick('reflections'), pick('truth'))
    O = events(pick('shadows'), pick('native'))
    total = D.size
    lines = [f'events (pixel × frame): {total}']
    pct = lambda x: f'{x.sum() / total * 100:.3f} %'
    lines.append(f'jumps, shadows + reflections: {pct(D)}')
    lines.append(f'jumps, reflections alone: {pct(R)}; shadows alone: {pct(O)}')
    only_both = D & ~R & ~O
    lines.append(f'jumps of both explained by shadows alone: {pct(D & O)}; by reflections alone: {pct(D & R & ~O)}')
    lines.append(f'jumps that only exist when both are on (overlap): {pct(only_both)} '
                 f'({only_both.sum() / max(D.sum(), 1) * 100:.1f} % of the jumps)')
    lines.append(f'jumps of shadows alone that disappear with the reflections: {pct(O & ~D)}')
    # floor: ground truth against native, where RT reflections touch nothing
    native_vs_truth = events(pick('native'), pick('truth'))
    lines.append(f'floor (native against ground truth: ideal reflections that move): {pct(native_vs_truth)}')
    text = '\n'.join(lines)
    print(text)
    with open(os.path.join(out, 'measurements.txt'), 'w', encoding='utf-8') as f:
        f.write(text + '\n')
    base = pick('both')[0] * 0.3
    k = max(1.0, len(D) * 0.2)
    base[..., 0] = np.maximum(base[..., 0], np.clip(only_both.sum(0) / k, 0, 1))
    base[..., 1] = np.maximum(base[..., 1], np.clip((D & O).sum(0) / k, 0, 1))
    base[..., 2] = np.maximum(base[..., 2], np.clip((D & R & ~O).sum(0) / k, 0, 1))
    Image.fromarray((base * 255).astype(np.uint8)).save(os.path.join(out, 'overlap.png'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
