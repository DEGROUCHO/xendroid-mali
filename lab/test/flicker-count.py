import sys, subprocess, tempfile, glob, numpy as np
from PIL import Image
for mp4 in sys.argv[1:]:
    d=tempfile.mkdtemp()
    subprocess.run(['ffmpeg','-loglevel','error','-i',mp4,'-vf','scale=480:-1',f'{d}/f%04d.png'],check=True)
    fr=[np.asarray(Image.open(p).convert('L'),dtype=np.float32) for p in sorted(glob.glob(f'{d}/*.png'))]
    if not fr: print(mp4,'no frames'); continue
    h,w=fr[0].shape; lum=np.array([f[int(h*.2):,int(w*.25):].mean() for f in fr])
    L=lum[np.r_[True,np.abs(np.diff(lum))>0.01]]
    dips=[i for i in range(1,len(L)-1) if L[i] < 0.6*min(L[i-1],L[i+1])]
    print(f'{mp4}: distinct {len(L)} black {(L<1).sum()} dips {len(dips)} lum_med {np.median(L):.1f}')
