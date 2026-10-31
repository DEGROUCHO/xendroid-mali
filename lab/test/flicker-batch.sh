#!/bin/bash
# flicker-batch.sh "TAG BUILD cvar..." ... : GTA moving with video for each line, then daily build + reference config.
cd "$(dirname "$0")"; A=/opt/homebrew/bin/adb
G=/sdcard/Android/data/xendroid.compose/files/compose/xenia-canary.config.toml
B=$(ls -t config-backups/*.toml | head -1)
pgrep -f keepawake.sh >/dev/null || (nohup bash keepawake.sh >/dev/null 2>&1 &)
for line in "$@"; do
  set -- $line; TAG=$1; shift
  rm -rf $TAG $TAG.log $TAG.out $TAG.mp4
  XD_MIN_BATTERY=${XD_MIN_BATTERY:-12} XD_ALLOW_OLD=1 bash flicker-run.sh $TAG "$@" > /dev/null 2>&1
  echo "$(tail -1 $TAG.out | cut -c1-260)"
  [ -f $TAG.mp4 ] && python3 flicker-count.py $TAG.mp4
done
bash restore-daily-on-reconnect.sh | tail -1
$A push $B $G >/dev/null; echo "config $($A shell md5sum $G | cut -c1-32) <- $B"
pkill -f keepawake.sh
