#!/bin/bash
# GTA in motion with full logcat in parallel: TAG BUILD [cvar...]
cd "$(dirname "$0")"; A=/opt/homebrew/bin/adb; TAG=$1; shift
G=/sdcard/Android/data/xendroid.compose/files/compose/xenia-canary.config.toml
B=$(ls -t config-backups/*.toml | head -1)
pgrep -f keepawake.sh >/dev/null || (nohup bash keepawake.sh >/dev/null 2>&1 &)
rm -rf $TAG $TAG.log $TAG.out
$A logcat -c; $A logcat -v threadtime > $TAG-logcat.txt 2>&1 &
lp=$!
XD_MIN_BATTERY=${XD_MIN_BATTERY:-12} XD_ALLOW_OLD=1 XD_LEAN=1 python3 xdtest.py $TAG "$@" --move --secs 60 > $TAG.out 2>&1
kill $lp
bash restore-daily-on-reconnect.sh | tail -1
$A push $B $G >/dev/null
pkill -f keepawake.sh
tail -1 $TAG.out | cut -c1-200
echo "outdated: $(grep -c 'become outdated' $TAG/xe-run.log) swapfail: $(grep -c 'Failed to create a swapchain' $TAG/xe-run.log)"
