#!/bin/bash
# usage: ui_build.sh <base.apk> <out.apk>
# Builds the UI working copy (this tree) around the native libe.so taken from <base.apk>,
# packaged as the release builds (app id XD_APP_ID, default xendroid.compose; non-debuggable, profileable).
# Only this tree is touched; build.gradle, the manifest and jniLibs are restored on exit.
set -euo pipefail
R="$(cd "$(dirname "$0")/.." && pwd)"
IN="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
OUT="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"
export XD_APP_ID="${XD_APP_ID:-xendroid.compose}"
cd "$R"
M=app/src/main/AndroidManifest.xml
SO=emulator-core/src/main/jniLibs/arm64-v8a/libe.so
B="$(mktemp -d /tmp/xd-ui-build.XXXXXX)"
cp app/build.gradle "$B/app.build.gradle"; cp "$M" "$B/AndroidManifest.xml"; cp "$SO" "$B/libe.so"
trap 'cp "$B/app.build.gradle" app/build.gradle && cp "$B/AndroidManifest.xml" "$M" && cp "$B/libe.so" "$SO" && rm -rf "$B"' EXIT
unzip -p "$IN" lib/arm64-v8a/libe.so > "$SO"
perl -pi -e "s/applicationId 'xendroid.compose'/applicationId '\$ENV{XD_APP_ID}'/; s/applicationIdSuffix '.debug'/\/\/ applicationIdSuffix '.debug'/" app/build.gradle
perl -pi -e "s/versionNameSuffix '-debug'/versionNameSuffix '-debug'\n            debuggable false/" app/build.gradle
perl -pi -e 's#android:theme="\@style/Theme.xendroidCompose">#android:theme="\@style/Theme.xendroidCompose">\n        <profileable android:shell="true" />#' "$M"
grep -q "applicationId '$XD_APP_ID'" app/build.gradle && grep -q "debuggable false" app/build.gradle && grep -q profileable "$M" || { echo EDIT_FAIL; exit 4; }
MAC_JDK=/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home
[ -d "$MAC_JDK" ] && export JAVA_HOME="$MAC_JDK"
./gradlew :app:assembleDebug --max-workers="${XD_WORKERS:-3}" -q
cp app/build/outputs/apk/debug/app-debug.apk "$OUT"
a=$(unzip -p "$IN" lib/arm64-v8a/libe.so | shasum -a 256 | cut -c1-16); b=$(unzip -p "$OUT" lib/arm64-v8a/libe.so | shasum -a 256 | cut -c1-16)
[ "$a" = "$b" ] && echo "ui build ok: native $b  $(shasum -a 256 "$OUT" | cut -c1-16)  $OUT" || { echo "LIBE MISMATCH"; exit 3; }
