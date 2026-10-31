#!/bin/bash
# arm.sh TAG APK DIAG [game] [secs]  - one short run on the migrated global (d988); restores best-apk + d988 at the end (harness finally)
cd /path/to/xendroid-ultimate-mali/lab/build/session-2026-10-09; L=../PHONE.lock; A=/opt/homebrew/bin/adb
tag=$1; apk=$2; diag=$3; game=${4:-Black Ops}; secs=${5:-270}
ready() { [ ! -e $L ] && $A shell dumpsys activity activities | grep topResumedActivity | grep -Fq 'com.miui.home.launcher.Launcher' && [ -z "$($A shell pidof xendroid.compose:emu | tr -d '\r')" ]; }
until ready; do sleep 15; done
echo "perf-thread $tag since $(date)" > $L; trap 'rm -f $L' EXIT
XD_START_MAX=410 XD_MEASURE_FROM=${MEASURE_FROM:-150} XD_EXTRA_SHOTS=${XD_EXTRA_SHOTS:-150,210,240,270} python3 -u pkg_run_c.py "$tag" "$apk" "$game:$secs" "$diag" $secs 2>&1 | grep -v "^INPUT" | tee arm-$tag.log
$A shell am force-stop xendroid.compose
echo "[$tag] phone md5 $($A shell md5sum /sdcard/Android/data/xendroid.compose/files/compose/xenia-canary.config.toml | cut -c1-12) apk $(cat /path/to/xendroid-ultimate-mali/lab/build/multigame-core-audit/best-apk.txt)"
