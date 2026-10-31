#!/bin/bash
# termo.sh FILE SECONDS - every 10 s: time, battery temperature (tenths), thermal headroom, current CPU/GPU/SKIN temperatures
f=$1; n=$(( $2 / 10 )); : > $f
for i in $(seq 1 $n); do
  b=$(adb shell dumpsys battery | grep temperature | tr -dc 0-9)
  h=$(adb shell cmd thermalservice headroom 0 2>/dev/null | grep -o "[0-9.]*$")
  c=$(adb shell dumpsys thermalservice | awk '/Current temperatures from HAL/{f=1} f&&/mName=(CPU|GPU|SKIN)/{match($0,/mValue=[0-9.]+/); v=substr($0,RSTART+7,RLENGTH-7); match($0,/mName=[A-Z]+/); printf "%s=%s ", substr($0,RSTART+6,RLENGTH-6), v}')
  echo "$(date +%H:%M:%S) batt=$b headroom=$h $c" >> $f
  sleep 9
done
