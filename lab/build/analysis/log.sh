#!/bin/bash
# usage: log.sh "title" < body   -> appends an entry with the real time
F=/path/to/xendroid-ultimate-mali/lab/build/analysis/CRONOLOGICO.md
printf '\n### %s — %s\n' "$(date +%H:%M)" "$1" >> "$F"; cat >> "$F"
