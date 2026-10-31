#!/bin/bash
# Build the current XenDroid source without discarding local Gradle/JNI changes.
set -euo pipefail
R="$(cd "$(dirname "$0")/../src/xendroid-src" && pwd)"
SDK="${ANDROID_HOME:-$HOME/Library/Android/sdk}"
HOST_TAG="$([ "$(uname)" = Darwin ] && echo darwin-x86_64 || echo linux-x86_64)"
NDK="$SDK/ndk/29.0.14206865/toolchains/llvm/prebuilt/$HOST_TAG/bin"
MAC_JDK=/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home
[ -d "$MAC_JDK" ] && export JAVA_HOME="$MAC_JDK"
WORKERS="${XD_WORKERS:-4}"
export USE_CCACHE=true
TMP="$(mktemp -d /tmp/xd-safe-build.XXXXXX)"
JNI="$R/emulator-core/src/main/jniLibs"
JNI_BAK="$R/emulator-core/src/main/jniLibs.safe-build-backup"
CORE_GRADLE="$R/emulator-core/build.gradle"
APP_GRADLE="$R/app/build.gradle"
RESTORED=0
restore() {
  [ "$RESTORED" = 1 ] && return
  cp "$TMP/emulator-core.build.gradle" "$CORE_GRADLE"
  cp "$TMP/app.build.gradle" "$APP_GRADLE"
  if [ -d "$JNI_BAK" ]; then
    if [ -e "$JNI" ]; then mv "$JNI" "$TMP/jni-generated"; fi
    mv "$JNI_BAK" "$JNI"
  fi
  cp "$TMP/libe-before.so" "$JNI/arm64-v8a/libe.so"
  RESTORED=1
  echo "BUILD CLEANUP: local Gradle and JNI files restored; backup $TMP"
}
cd "$R"
[ ! -e "$JNI_BAK" ] || { echo 'ABORT: existing JNI backup'; exit 2; }
cp "$CORE_GRADLE" "$TMP/emulator-core.build.gradle"
cp "$APP_GRADLE" "$TMP/app.build.gradle"
cp "$JNI/arm64-v8a/libe.so" "$TMP/libe-before.so"
trap 'st=$?; restore; [ "$st" = 0 ] && rm -rf "$TMP"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
mv "$JNI" "$JNI_BAK"
cp "$(dirname "$0")/build/core-gradle/emulator-core.build.gradle" "$CORE_GRADLE"
cp "$(dirname "$0")/build/core-gradle/app.build.gradle" "$APP_GRADLE"
./gradlew :emulator-core:assembleDebug --max-workers="$WORKERS"
SO="$(find emulator-core/build/intermediates/cxx -name libe.so -type f | head -1)"
[ -n "$SO" ] || { echo 'ABORT: libe.so absent'; exit 3; }
"$NDK/llvm-strip" --strip-all "$SO" -o /tmp/xd_libe_lateflow.so
cp "$TMP/emulator-core.build.gradle" "$CORE_GRADLE"
cp "$TMP/app.build.gradle" "$APP_GRADLE"
if [ -e "$JNI" ]; then mv "$JNI" "$TMP/jni-generated"; fi
mv "$JNI_BAK" "$JNI"
cp /tmp/xd_libe_lateflow.so "$JNI/arm64-v8a/libe.so"
./gradlew :app:assembleDebug --max-workers="$WORKERS"
cp app/build/outputs/apk/debug/app-debug.apk /tmp/xd_app_lateflow.apk
ls -lh /tmp/xd_app_lateflow.apk
