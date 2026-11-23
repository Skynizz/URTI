"""Count of RT reflection "popping" (snapshots of diagnostic view 9: share applied).

Toggle at (pixel, t): the RT reflection's share changes by more than 0.3 from one frame to the next.
Round trip: a toggle one way then the other way on the next frame (the reflection appears then
disappears at once, or the reverse): the fast blinking seen in the game.
Counts are relative to the pixels that receive an RT reflection (mean share > 0.05).
Output: toggles.png (red: accumulated round trips), measurements.txt.
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
    a = np.frombuffer(data[m.end():m.end() + w * h * 3], np.uint8).reshape(h, w, 3).astype(np.float32) / 255.0
    return a.mean(axis=2)


def main():
    folder = sys.argv[1]
    frames = sorted(int(m.group(1)) for m in (re.match(r'urti_photo_(\d+)\.ppm$', n) for n in os.listdir(folder)) if m)
    ims = [load(os.path.join(folder, f'urti_photo_{f}.ppm')) for f in frames]
    if len(ims) < 3:
        print('not enough snapshots')
        return 1
    # snapshots are encoded for the screen (sRGB or PQ): thresholds on the displayed value
    stack = np.stack(ims)
    covered = stack.mean(axis=0) > 0.05
    d = np.diff(stack, axis=0)
    flips = np.abs(d) > 0.3
    back = (d[1:] * d[:-1] < 0) & flips[1:] & flips[:-1]
    n = max(covered.sum(), 1)
    lines = [f'pixels with an RT reflection: {covered.mean() * 100:.1f} % of the image',
             f'toggles (share applied changing by more than 0.3): {flips[:, covered].mean() * 100:.3f} % of reflecting pixels per frame',
             f'round trips (appears then disappears at once): {back[:, covered].mean() * 100:.3f} % of reflecting pixels per frame']
    text = '\n'.join(lines)
    print(text)
    with open(os.path.join(folder, 'measurements.txt'), 'w', encoding='utf-8') as f:
        f.write(text + '\n')
    heat = np.clip(back.sum(axis=0) / max(1.0, len(back) * 0.2), 0, 1)
    img = np.stack([np.maximum(stack[0] * 0.4, heat), stack[0] * 0.4, stack[0] * 0.4], axis=2)
    Image.fromarray((img[::2, ::2] * 255).astype(np.uint8)).save(os.path.join(folder, 'toggles.png'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
