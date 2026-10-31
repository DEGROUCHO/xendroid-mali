#!/bin/bash
# q241: q240 + stencil-only and color+depth clear retargets.
# 1) BO 180 s (trace at 100 s) + Reach 240 s (menu); 2) Reach 240 s with the three new paths off (A/B for the pink menu).
cd "$(dirname "$0")"
until /opt/homebrew/bin/adb devices | grep -q "YOUR_DEVICE_SERIAL\s*device"; do sleep 10; done
echo "PHONE CONNECTED $(date +%H:%M:%S)"
REQ=/sdcard/Android/data/xendroid.compose/files/compose/rt-trace.request
DIAG='log_guest_fps=true,apu_aaudio_log_stats=true,vulkan_dxt1_to_astc=true,log_full_clear_transfer_skips=100,log_vk_memory=true,vulkan_rt_trace_request="'$REQ'"'
XD_TOUCH_AT=100 XD_TOUCH_PATH=$REQ python3 -u pkg_run.py q241-bundle q241-test.apk "Black Ops,Reach:240@200" "$DIAG" 180
XD_TOUCH_AT=200 XD_TOUCH_PATH=$REQ python3 -u pkg_run.py q241-reachoff q241-test.apk "Reach:240" \
  "$DIAG,GPU.rt_cache_draw_overwrite_cutouts=false,GPU.rt_cache_draw_clear_chains=false,GPU.rt_cache_retarget_depth_only_clears=false" 180
echo "ALL DONE $(date +%H:%M:%S)"
