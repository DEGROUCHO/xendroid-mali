#!/bin/bash
# Prints the current SoC temperature in whole degrees C (HAL value from
# dumpsys thermalservice, last "SOC" entry = current reading), or 99 if unknown.
ADB=${ADB_BIN:-/opt/homebrew/bin/adb}
v=$("$ADB" shell dumpsys thermalservice 2>/dev/null | grep -E 'mName=SOC' | grep 'Temperature{' | tail -1 | sed -E 's/.*mValue=([0-9]+)(\.[0-9]+)?.*/\1/')
echo "${v:-99}"
