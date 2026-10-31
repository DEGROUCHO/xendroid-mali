#!/bin/bash
# Prints the Mali GPU thermal cooling state (devfreq-13000000.mali in
# dumpsys thermalservice "Current cooling devices from HAL"): 0 = no GPU
# frequency cap, higher = max GPU OPP lowered by that many steps. 99 if unknown.
ADB=${ADB_BIN:-/opt/homebrew/bin/adb}
v=$("$ADB" shell dumpsys thermalservice 2>/dev/null | grep 'CoolingDevice{' | grep 'mali' | tail -1 | sed -E 's/.*mValue=([0-9]+).*/\1/')
echo "${v:-99}"
