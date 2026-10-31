#!/bin/bash
# After q241 BO: stop the q241 launcher before its Reach (no inputs), then run Reach to
# gameplay with the input schedule, new paths on and off (A/B).
cd "$(dirname "$0")"
until grep -q '^DONE {"game": "Call of Duty' q241-run.log; do sleep 5; done
pkill -f q241-wait-and-run.sh
P=$(pgrep -f "pkg_run.py q241-bundle"); [ -n "$P" ] && kill -INT $P
while pgrep -f "pkg_run.py q241-bundle" >/dev/null; do sleep 2; done
echo "BO DONE, Q241 BUNDLE STOPPED $(date +%H:%M:%S)"
REQ=/sdcard/Android/data/xendroid.compose/files/compose/rt-trace.request
DIAG='log_guest_fps=true,apu_aaudio_log_stats=true,vulkan_dxt1_to_astc=true,log_full_clear_transfer_skips=100,log_vk_memory=true,vulkan_rt_trace_request="'$REQ'"'
XD_TOUCH_PATH=$REQ python3 -u pkg_run.py q241-reach q241-test.apk "Reach:420@330" "$DIAG" 180
XD_TOUCH_PATH=$REQ python3 -u pkg_run.py q241-reachoff q241-test.apk "Reach:420@330" \
  "$DIAG,GPU.rt_cache_draw_overwrite_cutouts=false,GPU.rt_cache_draw_clear_chains=false,GPU.rt_cache_retarget_depth_only_clears=false" 180
echo "ALL DONE $(date +%H:%M:%S)"
