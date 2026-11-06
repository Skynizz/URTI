"""URTI : ce que l'effet change d'une image à l'autre sur une rafale (urti_rafale_N), prise de préférence le jeu en pause : l'image du jeu n'y
bouge presque plus, et tout ce qui varie dans la contribution d'URTI (après - avant) vient d'URTI.

    python urti_pause_ana.py <dossier de la rafale> <dossier de sortie>

Sortie : amplitude.png (image assombrie, en rouge ce qui varie), mesures sur la console.
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
idx=sorted(int(m[1]) for n in os.listdir(folder) if (m:=re.match(r'apres_(\d+)\.ppm$',n)))
A=np.stack([load(f'{folder}/avant_{i}.ppm').mean(2) for i in idx])
B=np.stack([load(f'{folder}/apres_{i}.ppm').mean(2) for i in idx])
C=B-A
print('images',len(idx),A.shape)
ra=A.max(0)-A.min(0); rb=B.max(0)-B.min(0); rc=C.max(0)-C.min(0)
print('amplitude sur la rafale (max-min), mediane/95/99 : jeu seul %.4f %.4f %.4f ; avec URTI %.4f %.4f %.4f ; contribution URTI %.4f %.4f %.4f'%(*np.percentile(ra,[50,95,99]),*np.percentile(rb,[50,95,99]),*np.percentile(rc,[50,95,99])))
for t in (0.02,0.05,0.1,0.2):
    print(f'pixels dont amplitude > {t}: jeu {100*(ra>t).mean():.2f} %  avec URTI {100*(rb>t).mean():.2f} %  contribution {100*(rc>t).mean():.2f} %')
sd=C.std(0)
print('écart-type dans le temps de la contribution : moyenne %.4f, 95%% %.4f'%(sd.mean(),np.percentile(sd,95)))
# spectre temporel : période dominante des pixels qui varient
mask=rc>0.05
if mask.sum():
    series=C[:,mask]; series=series-series.mean(0)
    sp=np.abs(np.fft.rfft(series,axis=0)).mean(1)
    print('spectre (période en images : amplitude) :',' '.join(f'{len(idx)/k:.1f}:{sp[k]:.3f}' for k in range(1,len(sp))))
# carte
g=lambda x:(np.clip(x,0,1)*255).astype(np.uint8)
img=np.stack([g(B[-1]*0.5+rc*3),g(B[-1]*0.5),g(B[-1]*0.5)],2)
Image.fromarray(img).save(f'{out}/amplitude.png')
Image.fromarray(g(rc*4)).save(f'{out}/amplitude_seule.png')
Image.fromarray(g(B[-1])).save(f'{out}/apres.png')
Image.fromarray(g(A[-1])).save(f'{out}/avant.png')
Image.fromarray(g(np.abs(C).mean(0)*4)).save(f'{out}/effet.png')
