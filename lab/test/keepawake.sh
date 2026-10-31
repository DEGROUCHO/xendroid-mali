#!/bin/bash
# Keep the phone awake during unattended runs: every 45 s, SHIFT when the launcher is focused (resets the
# user-activity timer), KEYCODE_WAKEUP if the screen is off and the keyguard is not showing. Never unlocks.
A=/opt/homebrew/bin/adb
while true; do
  if $A get-state >/dev/null 2>&1; then
    p=$($A shell dumpsys power 2>/dev/null)
    if echo "$p" | grep -q "mWakefulness=Asleep\|mWakefulness=Dozing"; then
      $A shell dumpsys window 2>/dev/null | grep -q "isStatusBarKeyguard=true\|mShowingLockscreen=true\|showing=true" || $A shell input keyevent KEYCODE_WAKEUP
    elif $A shell dumpsys window 2>/dev/null | grep -m1 mCurrentFocus | grep -qi launcher; then
      $A shell input keyevent KEYCODE_SHIFT_LEFT
    fi
  fi
  sleep 45
done
