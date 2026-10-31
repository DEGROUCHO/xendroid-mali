#!/bin/bash
# usage: analyze_prof.sh RUN_DIR SYMFS_DIR : simpleperf report-sample of RUN_DIR/perf.data(.gz) with symbols from
# SYMFS_DIR (libe.so with symtab) + JIT map RUN_DIR/perf-*.map -> RUN_DIR/report.txt and RUN_DIR/threads.txt
# (prof_threads.py per-thread on/off-CPU summary). Guest groups get detail.
set -e
R=$1; S=$2; SP=~/Library/Android/sdk/ndk/29.0.14206865/simpleperf/bin/darwin/x86_64/simpleperf
T="$(dirname "$0")"
[ -f "$R/perf.data" ] || gunzip -k "$R/perf.data.gz"
"$SP" report-sample -i "$R/perf.data" --show-callchain --symdir "$S" > "$R/report.txt" 2> "$R/report.err" || true
MAP=$(ls "$R"/perf-*.map 2>/dev/null | head -1)
python3 "$T/prof_threads.py" "$R/report.txt" $MAP --group-detail "Guest CPU" > "$R/threads.txt"
python3 "$T/driver_frames.py" "$R/report.txt" > "$R/driver-frames.txt"
wc -l "$R/report.txt" "$R/threads.txt" "$R/driver-frames.txt" | head -3
