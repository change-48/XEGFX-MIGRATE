#!/bin/bash
# A-88 P1 sweep: X us per commit -> p50/p90, two rounds each, plus one off-main-thread control point.
# usage: sweep1.sh [start_index]
set -uo pipefail
cd "$HOME/xegfx-log/A-88" || exit 1
CAF=""
caffeinate -u -t 2400 & CAF=$!
trap 'kill $CAF 2>/dev/null' EXIT

SCHED="
base2 off
base3 off
s1000a 1000m
s1000b 1000m
s2000a 2000m
s2000b 2000m
s3000a 3000m
s3000b 3000m
s5000a 5000m
s5000b 5000m
s8000a 8000m
s8000b 8000m
w3000a 3000w
w3000b 3000w
"

echo "$SCHED" | while read -r tag spin; do
  [ -z "${tag:-}" ] && continue
  echo "########## $tag $spin  $(date +%H:%M:%S)"
  bash ./run-a88.sh "$tag" "$spin" sample 2>&1 | grep -E "^(==|p50|p90|p99|mean|n |xa82load|flip_latch|early_|accel_kext|spin_file|plugin)"
done
echo "########## sweep done $(date +%H:%M:%S)"
