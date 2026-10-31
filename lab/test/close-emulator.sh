#!/bin/bash
# Closes XenDroid completely after a test: force-stop, Home, remove its tasks
# from Recents, then verify no process is left.
A=/opt/homebrew/bin/adb
$A shell am force-stop xendroid.compose
$A shell input keyevent KEYCODE_HOME
for tid in $($A shell "dumpsys activity recents" | grep -oE 'Task\{[0-9a-f]+ #[0-9]+ type=standard A=[0-9]+:(xendroid)' | grep -oE '#[0-9]+' | tr -d '#'); do
  $A shell "am stack remove $tid" >/dev/null 2>&1
done
left=$($A shell pidof xendroid.compose xendroid.compose:emu | tr -d '\r')
rec=$($A shell "dumpsys activity recents" | grep -c "xendroid")
echo "close-emulator: processes='${left}' recents=${rec}"
