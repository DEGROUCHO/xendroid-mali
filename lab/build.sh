#!/bin/bash
# Builds the XenDroid Ultimate Mali APK from this source tree (macOS or Linux).
# Usage: lab/build.sh                 -> lab/build/apk/xendroid-mali.apk
#        XD_APP_ID=my.app.id lab/build.sh   (different Android app id)
# Needs: Android SDK (ANDROID_HOME) with NDK 29.0.14206865 and CMake 3.30.3, JDK 21.
#   1. native core libe.so (lab/xd_build_safe.sh, the slow part: 20-60 min the first time)
#   2. the Compose UI app packaged around that core (src/xendroid-ui/tools/ui_build.sh)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/lab/build/apk"
mkdir -p "$OUT"
for d in src/xendroid-src src/xendroid-ui; do
  [ -f "$ROOT/$d/local.properties" ] || echo "sdk.dir=${ANDROID_HOME:-$HOME/Library/Android/sdk}" > "$ROOT/$d/local.properties"
done
rm -f /tmp/xd_app_lateflow.apk
bash "$ROOT/lab/xd_build_safe.sh"
cp /tmp/xd_app_lateflow.apk "$OUT/core.apk"
(cd "$ROOT/src/xendroid-ui" && bash tools/ui_build.sh "$OUT/core.apk" "$OUT/xendroid-mali.apk")
echo "APK ready: $OUT/xendroid-mali.apk"
