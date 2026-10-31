#!/bin/bash
# q238 package: draw overwrite cutouts + touch controls. BO 180 s, Reach 420 s (gameplay).
cd "$(dirname "$0")"
until /opt/homebrew/bin/adb devices | grep -q "YOUR_DEVICE_SERIAL\s*device"; do sleep 10; done
echo "PHONE CONNECTED $(date +%H:%M:%S)"
XD_TOUCH_AT=100 XD_TOUCH_PATH=/sdcard/Android/data/xendroid.compose/files/compose/rt-trace.request \
python3 -u pkg_run.py q238-bundle q238-test.apk "Black Ops,Reach:420@330" \
  'log_guest_fps=true,apu_aaudio_log_stats=true,vulkan_dxt1_to_astc=true,log_full_clear_transfer_skips=200,GPU.log_draw_overwrite_cutouts=200,log_vk_memory=true,vulkan_rt_trace_request="/sdcard/Android/data/xendroid.compose/files/compose/rt-trace.request"' 180
