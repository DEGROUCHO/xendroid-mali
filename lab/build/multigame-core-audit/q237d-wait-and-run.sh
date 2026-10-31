#!/bin/bash
# q237 diagnostic pass: Reach (not yet measured) + Black Ops, with the
# full-clear rejection log and the Vulkan memory accounting enabled.
cd "$(dirname "$0")"
until /opt/homebrew/bin/adb devices | grep -q "YOUR_DEVICE_SERIAL\s*device"; do sleep 10; done
echo "PHONE CONNECTED $(date +%H:%M:%S)"
XD_TOUCH_AT=100 XD_TOUCH_PATH=/sdcard/Android/data/xendroid.compose/files/compose/rt-trace.request \
python3 -u pkg_run.py q237-diag q237-test.apk "Reach,Black Ops" \
  'log_guest_fps=true,apu_aaudio_log_stats=true,vulkan_dxt1_to_astc=true,log_full_clear_transfer_skips=400,log_vk_memory=true,vulkan_rt_trace_request="/sdcard/Android/data/xendroid.compose/files/compose/rt-trace.request"' 180
