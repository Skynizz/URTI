"""URTI: what the effect changes from one frame to the next in a burst (urti_burst_N), preferably taken with the game paused: the game's
image barely moves there, and everything that varies in URTI's contribution (after - before) comes from URTI.

    python urti_pause_ana.py <burst folder> <output folder>

Output: amplitude.png (darkened image, what varies in red), measurements on the console.
"""
import sys,re,os
import numpy as np
from PIL import Image
def load(p):
    d=open(p,'rb').read()
    m=re.match(rb'P6\s+(\d+)\s+(\d+)\s+(\d+)\s',d)
    w,h=int(m[1]),int(m[2])
    return np.frombuffer(d[m.end():m.end()+w*h*3],np.uint8).reshape(h,w,3).astype(np.float32)/255
folder=sys.argv[1]; out=sys.argv[2]; os.makedirs(out,exist_ok=True)
idx=sorted(int(m[1]) for n in os.listdir(folder) if (m:=re.match(r'after_(\d+)\.ppm$',n)))
A=np.stack([load(f'{folder}/before_{i}.ppm').mean(2) for i in idx])
B=np.stack([load(f'{folder}/after_{i}.ppm').mean(2) for i in idx])
C=B-A
print('frames',len(idx),A.shape)
ra=A.max(0)-A.min(0); rb=B.max(0)-B.min(0); rc=C.max(0)-C.min(0)
print('amplitude over the burst (max-min), median/95/99: game alone %.4f %.4f %.4f; with URTI %.4f %.4f %.4f; URTI contribution %.4f %.4f %.4f'%(*np.percentile(ra,[50,95,99]),*np.percentile(rb,[50,95,99]),*np.percentile(rc,[50,95,99])))
for t in (0.02,0.05,0.1,0.2):
    print(f'pixels with amplitude > {t}: game {100*(ra>t).mean():.2f} %  with URTI {100*(rb>t).mean():.2f} %  contribution {100*(rc>t).mean():.2f} %')
sd=C.std(0)
print('standard deviation of the contribution over time: mean %.4f, 95%% %.4f'%(sd.mean(),np.percentile(sd,95)))
# temporal spectrum: dominant period of the varying pixels
mask=rc>0.05
if mask.sum():
    series=C[:,mask]; series=series-series.mean(0)
    sp=np.abs(np.fft.rfft(series,axis=0)).mean(1)
    print('spectrum (period in frames: amplitude):',' '.join(f'{len(idx)/k:.1f}:{sp[k]:.3f}' for k in range(1,len(sp))))
# map
g=lambda x:(np.clip(x,0,1)*255).astype(np.uint8)
img=np.stack([g(B[-1]*0.5+rc*3),g(B[-1]*0.5),g(B[-1]*0.5)],2)
Image.fromarray(img).save(f'{out}/amplitude.png')
Image.fromarray(g(rc*4)).save(f'{out}/amplitude_only.png')
Image.fromarray(g(B[-1])).save(f'{out}/after.png')
Image.fromarray(g(A[-1])).save(f'{out}/before.png')
Image.fromarray(g(np.abs(C).mean(0)*4)).save(f'{out}/effect.png')
