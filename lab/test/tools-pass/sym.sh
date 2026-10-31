#!/bin/bash
# sym.sh qNNN ADDR... : symbolize module offsets (hex, no 0x) with the build's snapshot symbols.
SN=/path/to/xendroid-ultimate-mali/lab/build/snapshots; C="$(dirname "$0")/symcache"; mkdir -p "$C"
b=$1; shift; so="$C/$b.so"; [ -s "$so" ] || xz -dc "$SN/$b/libe.sym.so.xz" > "$so"
for a in "$@"; do printf "%s " "$a"; /opt/homebrew/opt/llvm/bin/llvm-symbolizer --obj="$so" "0x$a" | head -1 | sed 's/(.*//' | cut -c1-110; done
