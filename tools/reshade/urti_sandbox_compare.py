"""URTI sandbox: compares a launch with RT and a native launch, frame by frame.

    python urti_sandbox_compare.py <RT folder> <native folder> <output folder>

Both folders hold urti_photo_<present>.ppm at the same presents (the sandbox is
deterministic: same camera, same wind, same passers-by). Measurements:
  - effect: mean difference between RT and native (what RT changes in the image);
  - flicker: mean difference from one frame to the next; the native launch gives the scene's own
    (wind, passers-by, camera); "added" = RT - native, what jumps because of URTI;
  - jumping points: share of pixels whose frame-to-frame change differs by more than
    8 % from the reference's at the same place (shadows or reflections that blink). The reference is
    the native launch, or the ground truth (urti_sandbox.sh --truth): the mean difference is then the error of
    the RT reflections, and a reflection that moves normally does not count as a jump.
Output: comparison.png (native | RT | amplified difference) for the first frame, flicker.png (map
of added jumps, accumulated over the series), measurements.txt.
"""
import os
import re
import sys

import numpy as np
from PIL import Image


def load(path):
    with open(path, 'rb') as f:
        data = f.read()
    # P6 header: width height maximum
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
    rt_dir, native_dir, out_dir = sys.argv[1:4]
    os.makedirs(out_dir, exist_ok=True)
    rt, native = frames(rt_dir), frames(native_dir)
    common = sorted(set(rt) & set(native))
    if not common:
        print('no common snapshot')
        return 1
    lines = []
    effect = []
    prev_rt = prev_nat = None
    flick_rt, flick_nat, jumps, blinks = [], [], [], []
    prev_d = None
    jump_map = None
    for i, f in enumerate(common):
        a, b = load(rt[f]), load(native[f])
        effect.append(float(np.abs(a - b).mean()))
        if i == 0:
            diff = np.clip(np.abs(a - b) * 4, 0, 1)
            Image.fromarray((np.concatenate([b, a, diff], axis=1) * 255).astype(np.uint8)).save(os.path.join(out_dir, 'comparison.png'))
            Image.fromarray((a * 255).astype(np.uint8)).save(os.path.join(out_dir, f'rt_{f}.png'))
            Image.fromarray((b * 255).astype(np.uint8)).save(os.path.join(out_dir, f'native_{f}.png'))
        if prev_rt is not None and f == common[i - 1] + 1:
            d_rt = np.abs(a - prev_rt).mean(axis=2)
            d_nat = np.abs(b - prev_nat).mean(axis=2)
            flick_rt.append(float(d_rt.mean()))
            flick_nat.append(float(d_nat.mean()))
            # change specific to RT: what changes from one frame to the next differently from the reference
            extra = np.abs((a - prev_rt) - (b - prev_nat)).mean(axis=2) > 0.08
            jumps.append(float(extra.mean()))
            jump_map = extra.astype(np.float32) if jump_map is None else jump_map + extra
            # blinking: the RT-specific difference changes sign from one frame to the next (brighter then
            # darker, or the reverse); an error that moves with the scene keeps the same sign
            d = ((a - prev_rt) - (b - prev_nat)).mean(axis=2)
            if prev_d is not None:
                blinks.append(float(((d * prev_d < 0) & (np.abs(d) > 0.04) & (np.abs(prev_d) > 0.04)).mean()))
            prev_d = d
        else:
            prev_d = None
        prev_rt, prev_nat = a, b
    lines.append(f'snapshots compared: {len(common)} ({common[0]} to {common[-1]})')
    lines.append(f'RT effect (mean difference RT / reference): {np.mean(effect) * 100:.2f} %')
    if flick_rt:
        fr, fn = np.mean(flick_rt), np.mean(flick_nat)
        lines.append(f'flicker from one frame to the next: RT {fr * 100:.3f} %, native {fn * 100:.3f} %, added {(fr - fn) * 100:+.3f} %')
        lines.append(f'jumping points (change departing from the reference by more than 8 %): {np.mean(jumps) * 100:.3f} % of pixels, at worst {np.max(jumps) * 100:.3f} %')
        if blinks:
            lines.append(f'blinking points (difference changing sign, more than 4 %): {np.mean(blinks) * 100:.3f} % of pixels')
        heat = np.clip(jump_map / max(1.0, len(jumps) * 0.25), 0, 1)
        base = load(rt[common[0]]) * 0.35
        base[..., 0] = np.maximum(base[..., 0], heat)
        Image.fromarray((base * 255).astype(np.uint8)).save(os.path.join(out_dir, 'flicker.png'))
    text = '\n'.join(lines)
    print(text)
    with open(os.path.join(out_dir, 'measurements.txt'), 'w', encoding='utf-8') as f:
        f.write(text + '\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
