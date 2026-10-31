#!/bin/bash
# Anti-pause guard: if the app's pause menu is open (green Resume button),
# tap Resume and re-check. Usage: xd_pauseguard.sh <label>
A="$HOME/Library/Android/sdk/platform-tools/adb"
S=YOUR_DEVICE_SERIAL
LABEL="${1:-guard}"
for try in 1 2; do
  "$A" -s "$S" exec-out screencap -p > /tmp/guard_$LABEL.png 2>/dev/null
  PAUSED=$(python3 -c "
from PIL import Image
import numpy as np
a=np.asarray(Image.open('/tmp/guard_$LABEL.png').convert('RGB')).astype(int)
r=a[950:1080,500:1100]
g=(r[:,:,1]>170)&(r[:,:,0]<170)&(r[:,:,2]<170)
print('PAUSED' if g.mean()>0.30 else 'PLAYING')
" 2>/dev/null)
  echo "pauseguard($LABEL) try$try: $PAUSED"
  if [ "$PAUSED" = "PAUSED" ]; then
    "$A" -s "$S" shell input tap 640 1020
    sleep 4
  else
    break
  fi
done
