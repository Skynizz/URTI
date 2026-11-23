"""Temporal noise URTI adds in a burst (before_k / after_k .ppm): frame-to-frame standard deviation, with URTI minus without, per pixel.
burst_noise.py <folder> [<folder> ...]  ->  map <folder>/noise.png; numbers: mean, share of pixels above 3 levels, 99th percentile."""
import glob
import os
import sys

import numpy as np
from PIL import Image


def stack(d, which):
    fs = sorted(glob.glob(os.path.join(d, which + '_*.ppm')), key=lambda s: int(s.rsplit('_', 1)[1].split('.')[0]))
    return np.stack([np.asarray(Image.open(f)).astype(np.float32).mean(-1) for f in fs])


for d in sys.argv[1:]:
    a, b = stack(d, 'before'), stack(d, 'after')
    # difference from one frame to the next (not the global standard deviation: a slow drift of the scene is not noise)
    da = np.abs(np.diff(a, axis=0)).mean(0)
    db = np.abs(np.diff(b, axis=0)).mean(0)
    ex = db - da
    Image.fromarray((np.clip(ex / 4, 0, 1) * 255).astype(np.uint8)).save(os.path.join(d, 'noise.png'))
    print('%-28s frames %d | game variation %.3f, with URTI %.3f | mean excess %.3f, pixels > 3 levels %.2f %%, 99th percentile %.2f' % (
        os.path.basename(d.rstrip('/\\')), len(a), da.mean(), db.mean(), ex.mean(), (ex > 3).mean() * 100, np.percentile(ex, 99)))
