#!/bin/bash
# Reach rerun up to gameplay (420 s), after the q237 diagnostic pass ends.
cd "$(dirname "$0")"
while pgrep -f "pkg_run.py q237-diag" >/dev/null; do sleep 15; done
echo "DIAG PASS ENDED $(date +%H:%M:%S)"
XD_TOUCH_AT=330 XD_TOUCH_PATH=/sdcard/Android/data/xendroid.compose/files/compose/rt-trace.request \
python3 -u pkg_run.py q237-reach q237-test.apk "Reach:420" \
  'log_guest_fps=true,apu_aaudio_log_stats=true,vulkan_dxt1_to_astc=true,log_full_clear_transfer_skips=400,log_vk_memory=true,vulkan_rt_trace_request="/sdcard/Android/data/xendroid.compose/files/compose/rt-trace.request"' 180
