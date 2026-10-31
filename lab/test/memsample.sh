#!/bin/bash
# memsample.sh OUT SECS: dumpsys meminfo summary of the emulator process every 2 s (diagnostic beside an xdtest run)
A=/opt/homebrew/bin/adb; end=$(( $(date +%s) + $2 ))
while [ $(date +%s) -lt $end ]; do
  p=$($A shell pidof xendroid.compose:emu 2>/dev/null)
  if [ -n "$p" ]; then echo "== $(date +%T) pid $p"; $A shell dumpsys meminfo $p 2>/dev/null | grep -E "Native Heap|Graphics|GL mtrack|EGL mtrack|Other mmap|Unknown|Other dev|\.so mmap|TOTAL PSS|TOTAL RSS|Gfx dev|Ashmem" | head -14; fi
  sleep 2
done
