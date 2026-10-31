#!/bin/bash
# replay.sh TAG APK XTR_PHONE_PATH [DIAG] [secs]  - loop playback of a recorded frame (.xtr).
# No game started, no input. Measures from second 20. Identical scene in every run.
S=/path/to/xendroid-ultimate-mali/lab/build/analysis
tag=$1; apk=$2; xtr=$3; diag=${4:-}; secs=${5:-60}
name=$(basename "$xtr" .xtr)
XD_NO_INPUTS=1 XD_EXTRA_PATHS="$xtr" MEASURE_FROM=20 XD_EXTRA_SHOTS=40 bash $S/arm.sh "$tag" "$apk" "GPU.log_guest_fps=true${diag:+,$diag}" "$name" "$secs"
