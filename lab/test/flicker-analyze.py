import sys, subprocess, numpy as np, tempfile, glob, os
from PIL import Image
for mp4 in sys.argv[1:]:
    d = tempfile.mkdtemp()
    subprocess.run(['ffmpeg','-loglevel','error','-i',mp4,'-vf','scale=480:-1',f'{d}/f%04d.png'],check=True)
    fr = [np.asarray(Image.open(p).convert('L'),dtype=np.float32) for p in sorted(glob.glob(f'{d}/*.png'))]
    h, w = fr[0].shape
    fr = [f[int(h*0.2):, int(w*0.25):] for f in fr]  # bottom/right of the fps overlay
    d1 = np.array([np.abs(fr[i]-fr[i-1]).mean() for i in range(2,len(fr))])
    d2 = np.array([np.abs(fr[i]-fr[i-2]).mean() for i in range(2,len(fr))])
    lum = np.array([f.mean() for f in fr])
    # duplicate frames (stutters) and flickers (d1 large, d2 small)
    dup = int((d1 < 0.05).sum())
    flick = int(((d1 > 3) & (d2 < d1*0.5)).sum())
    jump = np.abs(np.diff(lum))
    print(f'{os.path.basename(mp4)}: frames {len(fr)} lum {lum.mean():.1f} max_lum_jump {jump.max():.1f} '
          f'n_lum_jump>4 {(jump>4).sum()} flicker_frames {flick} dup_frames {dup} d1_med {np.median(d1):.2f} d2_med {np.median(d2):.2f}')
