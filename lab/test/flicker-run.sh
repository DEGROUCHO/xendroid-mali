#!/bin/bash
# One GTA run in motion with an 8 s video recorded with the game already started: TAG BUILD [cvar...]
cd "$(dirname "$0")"; A=/opt/homebrew/bin/adb; TAG=$1; shift
XD_LEAN=1 python3 xdtest.py $TAG "$@" --move --secs 55 > $TAG.out 2>&1 &
pid=$!
for i in $(seq 1 600); do grep -q "restore took" $TAG.log 2>/dev/null && break; sleep 2; done
sleep 20
$A shell screenrecord --time-limit 8 --bit-rate 20000000 /sdcard/Download/$TAG.mp4
$A pull /sdcard/Download/$TAG.mp4 $TAG.mp4 >/dev/null && $A shell rm /sdcard/Download/$TAG.mp4
wait $pid
tail -3 $TAG.out
