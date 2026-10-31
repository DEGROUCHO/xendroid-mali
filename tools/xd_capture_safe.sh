#!/usr/bin/env bash
# One XenDroid guest-output capture. Restores the previous APK and title config.
set -uo pipefail

ADB="${ADB:-$HOME/Library/Android/sdk/platform-tools/adb}"
SERIAL="${ANDROID_SERIAL:-YOUR_DEVICE_SERIAL}"
PKG="${XD_PKG:-xendroid.compose.debug}"
ACTIVITY=xendroid.compose.EmulatorHostActivity
TITLE_ID=41560817
BASE="/sdcard/Android/data/$PKG/files/compose"
REMOTE_CFG="$BASE/config/$TITLE_ID.config.toml"
MARKER="$BASE/guest-output.request"
SOURCE_MARKER="$BASE/source-rt.request"
TRACE_MARKER="$BASE/rt-trace.request"
CFG="${1:?usage: xd_capture_safe.sh q8.config.toml diagnostic.apk original.apk tag}"
DIAG_APK="${2:?diagnostic.apk required}"
ORIGINAL_APK="${3:?original.apk required}"
TAG="${4:?tag required}"
BOOT_LIMIT="${BOOT_LIMIT:-600}"
CAPTURE_LIMIT="${CAPTURE_LIMIT:-90}"
WATCHDOG_DELAY="${WATCHDOG_DELAY:-900}"
LOCK=/tmp/xd_capture_safe.lock
PAUSEGUARD="${PAUSEGUARD:-/tmp/xd_pauseguard.sh}"
ARTIFACTS="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/evidence/2026-09-27-guest-output/$TAG"

[[ "$TAG" =~ ^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$ ]] || { echo 'ABORT: unsafe tag'; exit 2; }
[[ -r "$CFG" && -r "$DIAG_APK" && -r "$ORIGINAL_APK" ]] || { echo 'ABORT: config/APK file missing'; exit 2; }
[[ -x "$ADB" && -x "$PAUSEGUARD" ]] || { echo 'ABORT: adb or pauseguard unavailable'; exit 2; }

SDK_ROOT="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$(cd "$(dirname "$ADB")/.." && pwd)}}"
AAPT="${AAPT:-}"
if [[ -z "$AAPT" ]]; then
  for candidate in "$SDK_ROOT"/build-tools/*/aapt; do
    [[ -x "$candidate" ]] && AAPT="$candidate"
  done
fi
[[ -x "$AAPT" ]] || { echo 'ABORT: Android aapt unavailable for APK identity check'; exit 2; }
apk_id() { "$AAPT" dump badging "$1" 2>/dev/null | sed -n "1s/^package: name='\([^']*\)'.*/\1/p"; }
[[ "$(apk_id "$DIAG_APK")" == "$PKG" && "$(apk_id "$ORIGINAL_APK")" == "$PKG" ]] || {
  echo "ABORT: both APKs must have package id $PKG"; exit 2;
}
python3 - "$CFG" "$MARKER" <<'PY' || exit 2
import sys, tomllib
with open(sys.argv[1], "rb") as f:
    config = tomllib.load(f)
if config.get("Vulkan", {}).get("vulkan_guest_output_capture_request") != sys.argv[2]:
    raise SystemExit("ABORT: config must set [Vulkan].vulkan_guest_output_capture_request to the guest-output.request path")
PY

SOURCE_CAPTURE="$(python3 - "$CFG" "$SOURCE_MARKER" <<'PYCFG'
import sys,tomllib
with open(sys.argv[1],'rb') as f: value=tomllib.load(f).get('Vulkan',{}).get('vulkan_source_rt_capture_request','')
if value and value != sys.argv[2]: raise SystemExit('ABORT: unexpected source capture marker')
print(int(bool(value)))
PYCFG
)" || exit 2

RT_TRACE="$(python3 - "$CFG" "$TRACE_MARKER" <<'PYCFG'
import sys,tomllib
with open(sys.argv[1],'rb') as f: value=tomllib.load(f).get('Vulkan',{}).get('vulkan_rt_trace_request','')
if value and value != sys.argv[2]: raise SystemExit('ABORT: unexpected rt trace marker')
print(int(bool(value)))
PYCFG
)" || exit 2

online() { [[ "$("$ADB" -s "$SERIAL" get-state 2>/dev/null)" == device ]]; }
temperature() {
  "$ADB" -s "$SERIAL" shell dumpsys battery 2>/dev/null |
    sed -n 's/^[[:space:]]*temperature: \([0-9]*\).*/\1/p' | tail -1 | tr -d '\r'
}
focused() {
  "$ADB" -s "$SERIAL" shell dumpsys activity activities 2>/dev/null |
    rg 'topResumedActivity=' | tail -1 | tr -d '\r'
}
pid_for() { "$ADB" -s "$SERIAL" shell pidof "$1" 2>/dev/null | tr -d '\r'; }
processes_absent() {
  local label="$1" base_pid emu_pid
  base_pid="$(pid_for "$PKG")"
  emu_pid="$(pid_for "$PKG:emu")"
  [[ -z "$base_pid" && -z "$emu_pid" ]] || {
    echo "ABORT: $label process remains (base=$base_pid emu=$emu_pid)"; return 1;
  }
}
guard() {
  online || { echo 'ABORT: ADB disconnected'; return 1; }
  local f t
  f="$(focused)"
  [[ "$f" == *"$PKG"* ]] || { echo "ABORT: focus changed: $f"; return 1; }
  t="$(temperature)"
  [[ "$t" =~ ^[0-9]+$ ]] && (( t < 445 )) || {
    echo "ABORT: battery temperature unavailable/high: $t"; return 1;
  }
}
frame_now() {
  "$ADB" -s "$SERIAL" shell "grep -oE 'f:[0-9]+' $BASE/xe.log 2>/dev/null | tail -1" |
    tr -d '\r' | cut -d: -f2
}

installed_hash() {
  local apk_path
  apk_path="$("$ADB" -s "$SERIAL" shell pm path "$PKG" 2>/dev/null | tr -d '\r' | sed -n 's/^package://p')"
  [[ "$apk_path" == /* && "$apk_path" != *$'\n'* ]] || return 1
  "$ADB" -s "$SERIAL" shell sha256sum "$apk_path" 2>/dev/null | awk '{print $1}'
}
ORIGINAL_HASH="$(shasum -a 256 "$ORIGINAL_APK" | awk '{print $1}')"

online || { echo 'ABORT: device absent'; exit 3; }
[[ "$(focused)" == *'com.miui.home.launcher.Launcher'* ]] || {
  echo "ABORT: phone not idle on launcher: $(focused)"; exit 3;
}
t="$(temperature)"
[[ "$t" =~ ^[0-9]+$ ]] && (( t <= 420 )) || {
  echo "ABORT: battery temperature unavailable/above 42 C at start: $t"; exit 3;
}
[[ -n "$("$ADB" -s "$SERIAL" shell pm path "$PKG" 2>/dev/null)" ]] || {
  echo "ABORT: original package $PKG is not installed"; exit 3;
}
processes_absent pre-install || exit 3
[[ "$(installed_hash)" == "$ORIGINAL_HASH" ]] || { echo 'ABORT: installed APK differs from restore APK'; exit 3; }
mkdir -p "$(dirname "$ARTIFACTS")" || exit 3
mkdir "$ARTIFACTS" 2>/dev/null || { echo "ABORT: artifacts already exist: $ARTIFACTS"; exit 3; }
mkdir "$LOCK" 2>/dev/null || { echo 'ABORT: another phone test holds lock'; exit 3; }
TMP="$(mktemp -d /tmp/xd_capture_safe.XXXXXX)" || exit 3
OLD_CFG="$ARTIFACTS/old-title-config.toml"
cp "$CFG" "$ARTIFACTS/tested-config.toml" || exit 3
CFG_EXISTED=0
CONFIG_CHANGED=0
INSTALL_ATTEMPTED=0
WATCHDOG_PID=""

cleanup() {
  local rc=$? restored=1
  trap - EXIT
  if online; then
    "$ADB" -s "$SERIAL" shell am force-stop "$PKG" >/dev/null 2>&1 || true
    if (( CONFIG_CHANGED )); then
      if (( CFG_EXISTED )); then
        "$ADB" -s "$SERIAL" push "$OLD_CFG" "$REMOTE_CFG" >/dev/null 2>&1 || { echo 'CLEANUP FAILED: title config restore failed'; rc=10; restored=0; }
      else
        "$ADB" -s "$SERIAL" shell rm -f "$REMOTE_CFG" >/dev/null 2>&1 || { echo 'CLEANUP FAILED: temporary title config remove failed'; rc=10; restored=0; }
      fi
    fi
    if (( INSTALL_ATTEMPTED )); then
      "$ADB" -s "$SERIAL" install -r "$ORIGINAL_APK" >"$ARTIFACTS/restore-apk.log" 2>&1 || { echo 'CLEANUP FAILED: original APK restore failed'; rc=10; restored=0; }
    fi
    if (( INSTALL_ATTEMPTED )) && [[ "$(installed_hash)" != "$ORIGINAL_HASH" ]]; then
      echo 'CLEANUP FAILED: restored APK hash mismatch'
      restored=0; rc=10
    fi
    if (( CONFIG_CHANGED && CFG_EXISTED )); then
      if ! "$ADB" -s "$SERIAL" pull "$REMOTE_CFG" "$ARTIFACTS/restored-config.toml" >/dev/null 2>&1 ||
          ! cmp -s "$OLD_CFG" "$ARTIFACTS/restored-config.toml"; then
        echo 'CLEANUP FAILED: restored config mismatch'
        restored=0; rc=10
      fi
    fi
    if [[ "$WATCHDOG_PID" =~ ^[0-9]+$ ]]; then
      "$ADB" -s "$SERIAL" shell kill "$WATCHDOG_PID" >/dev/null 2>&1 || true
    fi
    sleep 1
    if ! processes_absent cleanup; then
      echo 'CLEANUP FAILED: XenDroid process remains'
      rc=10
      restored=0
    fi
    if (( restored )); then
      echo 'CLEANUP: app stopped; original APK and title config restored'
    else
      echo "CLEANUP INCOMPLETE: recovery files retained in $ARTIFACTS and $TMP"
    fi
  else
    echo "CLEANUP PENDING: ADB disconnected; on-device watchdog stops the app after ${WATCHDOG_DELAY}s; APK/config restore awaits ADB"
    rc=11
  fi
  rmdir "$LOCK" 2>/dev/null || true
  rmdir "$TMP" 2>/dev/null || true
  exit "$rc"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

if "$ADB" -s "$SERIAL" shell test -f "$REMOTE_CFG"; then
  "$ADB" -s "$SERIAL" pull "$REMOTE_CFG" "$OLD_CFG" >/dev/null || exit 4
  CFG_EXISTED=1
fi
if "$ADB" -s "$SERIAL" shell test -f "$BASE/xe.log"; then
  "$ADB" -s "$SERIAL" pull "$BASE/xe.log" "$ARTIFACTS/xe-before.log" >/dev/null || exit 4
fi
WATCHDOG_PID="$("$ADB" -s "$SERIAL" shell "setsid sh -c 'sleep $WATCHDOG_DELAY; am force-stop $PKG' </dev/null >/dev/null 2>&1 & echo \$!" | tr -d '\r')"
[[ "$WATCHDOG_PID" =~ ^[0-9]+$ ]] || { echo 'ABORT: watchdog not armed'; exit 5; }
echo "RUN $TAG: temp=${t}/10 C, watchdog=$WATCHDOG_PID"

CONFIG_CHANGED=1
"$ADB" -s "$SERIAL" push "$CFG" "$REMOTE_CFG" >/dev/null || exit 6
"$ADB" -s "$SERIAL" shell rm -f "$MARKER" "$SOURCE_MARKER" "$TRACE_MARKER" "$BASE/xe.log" || exit 6
"$ADB" -s "$SERIAL" shell rm -rf "$BASE/texdump" "$BASE/texaudit" >/dev/null 2>&1 || true
if [[ "$(shasum -a 256 "$DIAG_APK" | awk '{print $1}')" == "$ORIGINAL_HASH" ]]; then
  echo "diagnostic APK identical to installed original: install skipped"
else
  INSTALL_ATTEMPTED=1
  "$ADB" -s "$SERIAL" install -r "$DIAG_APK" || exit 6
fi
"$ADB" -s "$SERIAL" shell am force-stop "$PKG" >/dev/null || exit 6
processes_absent post-install || exit 6
"$ADB" -s "$SERIAL" shell rm -f "$BASE/live.toml" "$BASE/live.toml.tmp"
"$ADB" -s "$SERIAL" shell "am start -n $PKG/$ACTIVITY -a xendroid.intent.action.xendroid --es game_uri /sdcard/ROMs/xbox360/mw2-360.iso --es title_id $TITLE_ID --es game_name 'Modern Warfare 2'" || exit 6

frame=''
for ((elapsed=0; elapsed<BOOT_LIMIT; elapsed+=5)); do
  sleep 5
  online || { echo 'ABORT: device disconnected during boot'; exit 7; }
  frame="$(frame_now)"
  if [[ -n "${FAST:-}" ]]; then
    # FAST: gameplay starts on its own - gate on a heavy frame instead of the
    # frame count, so the phone isn't heated by minutes of extra gameplay.
    draws="$("$ADB" -s "$SERIAL" shell "grep -oE 'GuestFps: [0-9.]+ fps, [0-9]+ draws' $BASE/xe.log 2>/dev/null | tail -1" | tr -d '\r' | awk '{print $4}')"
    if [[ "$draws" =~ ^[0-9]+$ ]] && (( draws >= 2000 )); then frame=99999; break; fi
  elif [[ "$frame" =~ ^[0-9]+$ ]] && (( 10#$frame > 1200 )); then break; fi
  if (( elapsed > 20 )); then guard || exit 7; fi
done
echo "BOOT frame=$frame"
[[ "$frame" =~ ^[0-9]+$ ]] && (( 10#$frame > 1200 )) || { echo 'INVALID: gameplay frame gate not reached'; exit 8; }
"$ADB" -s "$SERIAL" shell "grep -q 'Failed to parse config' $BASE/xe.log" && { echo 'INVALID: config parse failed'; exit 8; }

# Match the known menu taps and pause guard; this capture run intentionally has no swipes.
TAP_DELAYS="2 6 6 6 24 20 6"
[[ -n "${FAST:-}" ]] && TAP_DELAYS=""
for delay in $TAP_DELAYS; do
  guard || exit 9
  if (( delay == 2 )); then xy='1356 300'; else xy='2404 671'; fi
  "$ADB" -s "$SERIAL" shell input tap $xy || exit 9
  sleep "$delay"
done
guard || exit 9
pause_out="$("$PAUSEGUARD" "$TAG")" || exit 9
echo "$pause_out"
[[ "$pause_out" == *PLAYING* ]] || { echo 'INVALID: gameplay pause state unverified'; exit 9; }
if [[ -n "${EXTRA_WAIT:-}" ]]; then
  # Let the game run longer before the capture (texture degradation over time).
  for ((w=0; w<EXTRA_WAIT; w+=10)); do sleep 10; guard || exit 9; done
  echo "EXTRA_WAIT=${EXTRA_WAIT}s done"
fi
if [[ -n "${LIVE_AB:-}" ]]; then
  # Optional in-game A/B(/C...): LIVE_AB="<A.toml> <B.toml> ..." pushed in turn
  # to the live cvar file (config must set live_cvar_file = "$BASE/live.toml")
  # every LIVE_STEP seconds for LIVE_CYCLES cycles; the emulator logs "LiveCvar:".
  read -r -a live_files <<< "$LIVE_AB"
  for ((c=0; c<${LIVE_CYCLES:-3}; c++)); do
    for live_f in "${live_files[@]}"; do
      "$ADB" -s "$SERIAL" push "$live_f" "$BASE/live.toml.tmp" >/dev/null 2>&1 &&
        "$ADB" -s "$SERIAL" shell "mv '$BASE/live.toml.tmp' '$BASE/live.toml' && touch '$BASE/live.toml'" || exit 9
      echo "LIVE_AB $(date +%H:%M:%S) -> $(basename "$live_f")"
      sleep "${LIVE_STEP:-8}"
      guard || exit 9
    done
  done
  "$ADB" -s "$SERIAL" shell rm -f "$BASE/live.toml"
fi
if [[ -n "${SIMPLEPERF:-}" ]]; then
  # Optional CPU profile of the emulator process during gameplay (debuggable
  # app, no root): cpu-clock samples with frame-pointer call graphs.
  "$ADB" -s "$SERIAL" shell "rm -f /data/local/tmp/xd_perf.data; simpleperf record --app $PKG --duration $SIMPLEPERF ${SIMPLEPERF_EVENTS:--e cpu-clock -f 2000} --call-graph ${SIMPLEPERF_CG:-fp} -o /data/local/tmp/xd_perf.data" > "$ARTIFACTS/simpleperf.log" 2>&1 || true
  "$ADB" -s "$SERIAL" pull /data/local/tmp/xd_perf.data "$ARTIFACTS/perf.data" >/dev/null 2>&1 || true
  "$ADB" -s "$SERIAL" shell rm -f /data/local/tmp/xd_perf.data
  guard || exit 9
  echo "SIMPLEPERF=${SIMPLEPERF}s done"
fi
if [[ -n "${SCREENRECORD:-}" ]]; then
  # Optional short screen recording of the presented frames (frame generation
  # checks), with a slow camera swipe on the right half so there is motion.
  "$ADB" -s "$SERIAL" shell rm -f /sdcard/xd_screenrecord.mp4
  "$ADB" -s "$SERIAL" shell screenrecord --time-limit "$SCREENRECORD" /sdcard/xd_screenrecord.mp4 &
  rec_pid=$!
  sleep 1
  if [[ -n "${SCREENRECORD_SWIPE:-}" ]]; then
    "$ADB" -s "$SERIAL" shell input swipe 2000 800 2350 760 2500 || true
    sleep 1
    "$ADB" -s "$SERIAL" shell input swipe 2350 760 2000 800 2500 || true
  fi
  wait "$rec_pid" || true
  sleep 1
  "$ADB" -s "$SERIAL" pull /sdcard/xd_screenrecord.mp4 "$ARTIFACTS/screenrecord.mp4" >/dev/null 2>&1 || true
  "$ADB" -s "$SERIAL" shell rm -f /sdcard/xd_screenrecord.mp4
  guard || exit 9
  echo "SCREENRECORD=${SCREENRECORD}s done"
fi
frame_before="$(frame_now)"
sleep 4
guard || exit 9
frame_after="$(frame_now)"
[[ "$frame_before" =~ ^[0-9]+$ && "$frame_after" =~ ^[0-9]+$ ]] &&
  (( 10#$frame_after > 10#$frame_before )) || { echo 'INVALID: guest frame stopped before capture'; exit 9; }

# Preserve the visible Android surface immediately before the one-shot guest capture.
"$ADB" -s "$SERIAL" exec-out screencap -p > "$ARTIFACTS/screen-before.png" || exit 10
guard || exit 10
if [[ "$RT_TRACE" == 1 ]]; then
  # One-frame RTTRACE: armed at the next guest frame boundary, consumed by the app.
  "$ADB" -s "$SERIAL" shell touch "$TRACE_MARKER" || exit 10
  sleep 2
  echo "RT TRACE marker set"
fi
if [[ "$SOURCE_CAPTURE" == 1 ]]; then
  "$ADB" -s "$SERIAL" shell touch "$SOURCE_MARKER" || exit 10
  source_ok=0
  for ((elapsed=0; elapsed<CAPTURE_LIMIT; elapsed+=2)); do
    if (( elapsed % 10 == 0 )); then guard || exit 11; fi
    source_line="$("$ADB" -s "$SERIAL" shell "grep -F '[SOURCE-RT-CAPTURE]' $BASE/xe.log 2>/dev/null | tail -1" | tr -d '\r')"
    if [[ "$source_line" == *'status=error'* ]]; then
      echo "SOURCE CAPTURE FAILED: $source_line"
      "$ADB" -s "$SERIAL" pull "$BASE/xe.log" "$ARTIFACTS/xe-after.log" >/dev/null 2>&1 || true
      exit 11
    fi
    if [[ "$source_line" == *'status=ok'* ]]; then
      source_frame="$(sed -n 's/.*status=ok frame=\([0-9][0-9]*\).*/\1/p' <<<"$source_line")"
      [[ "$source_frame" =~ ^[0-9]+$ && "$source_line" == *"raw_path=$BASE/source-rt-frame-$source_frame.bin metadata_path=$BASE/source-rt-frame-$source_frame.json"* ]] || { echo "INVALID source record: $source_line"; exit 11; }
      for ext in bin json; do
        "$ADB" -s "$SERIAL" pull "$BASE/source-rt-frame-$source_frame.$ext" "$ARTIFACTS/source-rt-frame-$source_frame.$ext" >/dev/null || exit 11
      done
      for ext in bin json; do
        # Optional pre-capture (same frame, at the 1024x600 depth resolve).
        "$ADB" -s "$SERIAL" pull "$BASE/source-rt-pre-frame-$source_frame.$ext" "$ARTIFACTS/source-rt-pre-frame-$source_frame.$ext" >/dev/null 2>&1 || true
      done
      source_ok=1
      echo "$source_line"
      break
    fi
    sleep 2
  done
  [[ "$source_ok" == 1 ]] || {
    "$ADB" -s "$SERIAL" pull "$BASE/xe.log" "$ARTIFACTS/xe-after.log" >/dev/null 2>&1 || true
    echo "INVALID: source capture timeout (last: $source_line)"; exit 11;
  }
  "$ADB" -s "$SERIAL" exec-out screencap -p > "$ARTIFACTS/screen-after-source.png" || exit 11
fi
"$ADB" -s "$SERIAL" shell touch "$MARKER" || exit 10
echo "TRIGGERED $MARKER at frame=$frame_after"

capture_line=''
remote_png=''
capture_frame=''
for ((elapsed=0; elapsed<CAPTURE_LIMIT; elapsed+=2)); do
  if (( elapsed % 10 == 0 )); then guard || exit 11; fi
  capture_line="$("$ADB" -s "$SERIAL" shell "grep -F '[GUEST-OUTPUT-CAPTURE]' $BASE/xe.log 2>/dev/null | tail -1" | tr -d '\r')"
  if [[ "$capture_line" == *'status=error'* ]]; then
    echo "CAPTURE FAILED: $capture_line"
    "$ADB" -s "$SERIAL" pull "$BASE/xe.log" "$ARTIFACTS/xe-after.log" >/dev/null 2>&1 || true
    exit 11
  fi
  if [[ "$capture_line" == *'status=ok'* ]]; then
    capture_frame="$(sed -n 's/.*status=ok frame=\([0-9][0-9]*\).*/\1/p' <<<"$capture_line")"
    remote_png="$(sed -n 's/.* path=\([^ ]*\).*/\1/p' <<<"$capture_line")"
    [[ "$capture_frame" =~ ^[0-9]+$ && "$remote_png" == "$BASE/guest-output-pre-present-frame-$capture_frame.png" ]] || {
      echo "CAPTURE FAILED: unexpected success record: $capture_line"; exit 11;
    }
    if "$ADB" -s "$SERIAL" shell test -s "$remote_png"; then break; fi
  fi
  sleep 2
done
[[ -n "$remote_png" ]] && "$ADB" -s "$SERIAL" shell test -s "$remote_png" || {
  echo "INVALID: no successful guest PNG within ${CAPTURE_LIMIT}s (last: $capture_line)"; exit 11;
}
"$ADB" -s "$SERIAL" pull "$remote_png" "$ARTIFACTS/guest-output-pre-present-frame-$capture_frame.png" >/dev/null || exit 12
"$ADB" -s "$SERIAL" exec-out screencap -p > "$ARTIFACTS/screen-after.png" || exit 12
# Optional consecutive raw frames (vulkan_guest_output_capture_sequence > 1):
# pull the ones this run logged (artifact check for flashes / corruption).
seq_count="$(sed -n 's/^[[:space:]]*vulkan_guest_output_capture_sequence[[:space:]]*=[[:space:]]*\([0-9][0-9]*\).*/\1/p' "$CFG" | tail -1)"
if [[ "$seq_count" =~ ^[0-9]+$ ]] && (( seq_count > 1 )); then
  for ((t=0; t<30; t+=2)); do
    n="$("$ADB" -s "$SERIAL" shell "grep -cF '[GUEST-OUTPUT-SEQ] status=ok' $BASE/xe.log" 2>/dev/null | tr -d '\r')"
    [[ "$n" =~ ^[0-9]+$ ]] && (( n >= seq_count - 1 )) && break
    sleep 2
  done
  mkdir -p "$ARTIFACTS/seq"
  "$ADB" -s "$SERIAL" shell "grep -F '[GUEST-OUTPUT-SEQ] status=ok' $BASE/xe.log" 2>/dev/null | tr -d '\r' |
    sed -n 's/.* path=\([^ ]*\).*/\1/p' | while read -r seq_path; do
      "$ADB" -s "$SERIAL" pull "$seq_path" "$ARTIFACTS/seq/" >/dev/null 2>&1 || echo "WARN: seq pull failed $seq_path"
    done
fi
if grep -Eq '^[[:space:]]*vulkan_debug_texture_dump[[:space:]]*=[[:space:]]*true' "$CFG"; then
  for ((t=0; t<60; t+=2)); do
    if "$ADB" -s "$SERIAL" shell "grep -qF '[TEXDUMP]' $BASE/xe.log" 2>/dev/null; then break; fi
    sleep 2
  done
  "$ADB" -s "$SERIAL" pull "$BASE/texdump" "$ARTIFACTS/" >/dev/null 2>&1 || echo 'WARN: texdump pull failed'
fi
if grep -Eq '^[[:space:]]*vulkan_debug_texture_audit[[:space:]]*=[[:space:]]*true' "$CFG"; then
  for ((t=0; t<90; t+=2)); do
    if "$ADB" -s "$SERIAL" shell "grep -qF '[TEXAUDIT] status' $BASE/xe.log" 2>/dev/null; then break; fi
    sleep 2
  done
  "$ADB" -s "$SERIAL" pull "$BASE/texaudit" "$ARTIFACTS/" >/dev/null 2>&1 || echo 'WARN: texaudit pull failed'
fi
"$ADB" -s "$SERIAL" pull "$BASE/xe.log" "$ARTIFACTS/xe-after.log" >/dev/null || exit 12
"$ADB" -s "$SERIAL" logcat -d -t 30000 | rg 'GUEST-OUTPUT-CAPTURE|SOURCE-RT-CAPTURE|EmuHost.*fps' > "$ARTIFACTS/logcat-capture.txt" || true
echo "CAPTURED frame=$capture_frame: $ARTIFACTS/guest-output-pre-present-frame-$capture_frame.png"
