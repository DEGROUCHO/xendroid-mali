#!/bin/bash
# During a replay run: hardware counters of the CP thread ("GPU Commands") for 8 s. usage: cpstat.sh OUT
A=/opt/homebrew/bin/adb; P=xendroid.compose
until grep -q "q468prof start" /path/to/xendroid-ultimate-mali/lab/test/queue-prof467.out; do sleep 5; done
sleep 65
pid=$($A shell pidof $P:emu); tid=$($A shell "for t in /proc/$pid/task/*; do c=\$(cat \$t/comm); case \"\$c\" in GPU\ Commands*) basename \$t;; esac; done" | head -1)
echo "pid=$pid tid=$tid" > $1
$A shell "simpleperf stat --app $P -t $tid -e cpu-cycles,instructions,cache-misses,cache-references,stalled-cycles-backend,stalled-cycles-frontend,branch-misses --duration 8" >> $1 2>&1
