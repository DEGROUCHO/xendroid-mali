#!/bin/bash
# CPU profile summary of a pkg_run game directory with perf.data and perf-<pid>.map
# (XD_SIMPLEPERF_AT): per thread group on/off-CPU, top self frames with JIT code
# resolved through the perf map.
#   perfreport.sh <game dir> <qNNN>
D=$1; Q=$2
SP=$(ls ~/Library/Android/sdk/ndk/*/simpleperf/bin/darwin/x86_64/simpleperf | tail -1)
SYM="/path/to/xendroid-ultimate-mali/archive/artifacts/symfs/symfs_$Q"
"$SP" report-sample -i "$D/perf.data" --show-callchain --symdir "$SYM" > "$D/perf-report.txt" 2> "$D/perf-report.err"
MAP=$(ls "$D"/perf-*.map 2>/dev/null | head -1)
python3 "$(dirname "$0")/prof_threads.py" "$D/perf-report.txt" $MAP
