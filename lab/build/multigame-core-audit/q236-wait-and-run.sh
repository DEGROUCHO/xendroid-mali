#!/bin/bash
# Waits for the phone, then runs the single bundled q236 test.
cd "$(dirname "$0")"
until /opt/homebrew/bin/adb devices | grep -q "YOUR_DEVICE_SERIAL\s*device"; do sleep 10; done
echo "PHONE CONNECTED $(date +%H:%M:%S)"
XD_TOUCH_AT=100 XD_TOUCH_PATH=/sdcard/Android/data/xendroid.compose/files/compose/rt-trace.request \
python3 -u pkg_run.py q236-bundle q236-test.apk "Black Ops,Mass Effect,Reach" \
  'log_guest_fps=true,apu_aaudio_log_stats=true,vulkan_dxt1_to_astc=true,vulkan_rt_trace_request="/sdcard/Android/data/xendroid.compose/files/compose/rt-trace.request"' 180
