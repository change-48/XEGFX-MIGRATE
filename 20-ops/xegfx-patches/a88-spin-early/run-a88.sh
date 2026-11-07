#!/bin/bash
# A-88 P1: one 25 s drag with X us of injected work on one path, frame times, optional WindowServer sample.
# usage: run-a88.sh <tag> <off|1000m|1000w> [sample|nosample]
# Leaves the switch OFF at the end (a spin left running would make the machine crawl if the session dies).
set -uo pipefail
TAG=${1:?usage: run-a88.sh <tag> <off|Xm|Xw> [sample|nosample]}
SPIN=${2:-off}
DOSAMPLE=${3:-sample}
L=$HOME/xegfx-log/A-88/measure
S=/usr/sbin/sysctl
mkdir -p "$L"

setspin() {   # $1 = off | Xm | Xw
  if [ "$1" = off ]; then sudo rm -f /private/tmp/nvmtl-spin
  else printf '%s\n' "$1" | sudo tee /private/tmp/nvmtl-spin > /dev/null; fi
}

setspin off                        # marker 0: baseline counter, whatever came before
sleep 1
setspin "$SPIN"                    # marker 1: the counter this run starts from
sleep 1
echo "== state  tag=$TAG spin=$SPIN"
echo "spin_file   = $(cat /private/tmp/nvmtl-spin 2>/dev/null || echo off)"
echo "plugin      = $(shasum -a256 /Library/GPUBundles/NVMTLDriver.bundle/Contents/MacOS/NVMTLDriver | cut -c1-16)"
echo "accel_kext  = $(kmutil showloaded | grep -c XeGfxAccel)"
echo "flip=$($S -n debug.xegfxaccel_iop_xe_flip) defer=$($S -n debug.xegfxaccel_iop_xe_defer) wait_idle=$($S -n debug.xegfxaccel_iop_xe_wait_idle) flips=$($S -n debug.xegfxaccel_iop_xe_flips)"
F0=$($S -n debug.xegfxaccel_iop_xe_flips)

~/xegfx-log/A-82/tools/xa82load 25 1100 760 16 -b > "$L/load-$TAG.out" 2>&1 &
lp=$!
sleep 3
sudo -n "$HOME/xegfx-log/A-85/tools/frame-times" 16 220 > "$L/iv-$TAG.txt" 2>&1 &
fp=$!
if [ "$DOSAMPLE" = sample ]; then sleep 12; sudo -n /usr/bin/sample WindowServer 6 -file "$L/sample-$TAG.txt" > /dev/null 2>&1; fi
wait $fp 2>/dev/null
kill $lp 2>/dev/null
sleep 1
F1=$($S -n debug.xegfxaccel_iop_xe_flips)

echo "== screen"; head -2 "$L/load-$TAG.out"
echo "== frame times"; grep -E "^(p50|p90|p99|mean|n )" "$L/iv-$TAG.txt" 2>/dev/null | head -8
echo "== latch/early"
echo "flip_latch_us=$($S -n debug.xegfxaccel_iop_xe_flip_latch_us) arm_last_us=$($S -n debug.xegfxaccel_iop_xe_arm_last_us) latch_timeouts=$($S -n debug.xegfxaccel_iop_xe_latch_timeouts)"
echo "early_frames=$($S -n debug.xegfxaccel_iop_xe_early_frames) early_ready=$($S -n debug.xegfxaccel_iop_xe_early_ready) early_third=$($S -n debug.xegfxaccel_iop_xe_early_third)"
echo "== frames $F0 -> $F1  delta=$((F1-F0))"

setspin off                        # marker 2: the counter this run ends on; also the safety reset
sleep 1
echo "== spin markers (last 4)"
log show --last 4m --predicate 'eventMessage CONTAINS "A88 spin"' --style compact 2>/dev/null | tail -4
echo "== files"; ls -la "$L" | grep -- "-$TAG" | tail -4
