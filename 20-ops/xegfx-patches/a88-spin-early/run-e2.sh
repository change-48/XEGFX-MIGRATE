#!/bin/bash
# A-88 P2: one 25 s drag with the E2 early signal off or on, frame times, optional WindowServer sample.
# usage: run-e2.sh <tag> <0|1> [sample|nosample]
set -uo pipefail
TAG=${1:?usage: run-e2.sh <tag> <0|1> [sample|nosample]}
EARLY=${2:?0 or 1}
DOSAMPLE=${3:-sample}
L=$HOME/xegfx-log/A-88/measure
S=/usr/sbin/sysctl
mkdir -p "$L"

sudo rm -f /private/tmp/nvmtl-spin          # P1's switch must be out of the way
sudo $S -w debug.xegfxaccel_iop_xe_early=$EARLY > /dev/null
sleep 1
echo "== state  tag=$TAG early=$EARLY"
echo "early        = $($S -n debug.xegfxaccel_iop_xe_early)  signals=$($S -n debug.xegfxaccel_iop_xe_early_signals)"
echo "plugin       = $(shasum -a256 /Library/GPUBundles/NVMTLDriver.bundle/Contents/MacOS/NVMTLDriver | cut -c1-16)"
echo "accel_kext   = $(kmutil showloaded | grep -c XeGfxAccel)"
echo "accel_bin    = $(md5 -q /Library/Extensions/XeGfxAccel.kext/Contents/MacOS/XeGfxAccel | cut -c1-12)"
echo "flip=$($S -n debug.xegfxaccel_iop_xe_flip) defer=$($S -n debug.xegfxaccel_iop_xe_defer) wait_idle=$($S -n debug.xegfxaccel_iop_xe_wait_idle) flips=$($S -n debug.xegfxaccel_iop_xe_flips)"
E0=$($S -n debug.xegfxaccel_iop_xe_early_signals); F0=$($S -n debug.xegfxaccel_iop_xe_flips)

~/xegfx-log/A-82/tools/xa82load 25 1100 760 16 -b > "$L/load-$TAG.out" 2>&1 &
lp=$!
sleep 3
sudo -n "$HOME/xegfx-log/A-85/tools/frame-times" 16 220 > "$L/iv-$TAG.txt" 2>&1 &
fp=$!
if [ "$DOSAMPLE" = sample ]; then sleep 12; sudo -n /usr/bin/sample WindowServer 6 -file "$L/sample-$TAG.txt" > /dev/null 2>&1; fi
wait $fp 2>/dev/null
kill $lp 2>/dev/null
sleep 1
E1=$($S -n debug.xegfxaccel_iop_xe_early_signals); F1=$($S -n debug.xegfxaccel_iop_xe_flips)

echo "== screen"; head -2 "$L/load-$TAG.out"
echo "== latch/early"
echo "flip_latch_us=$($S -n debug.xegfxaccel_iop_xe_flip_latch_us) arm_last_us=$($S -n debug.xegfxaccel_iop_xe_arm_last_us) latch_timeouts=$($S -n debug.xegfxaccel_iop_xe_latch_timeouts) flip_fail=$($S -n debug.xegfxaccel_iop_xe_flip_fail)"
echo "early_ready=$($S -n debug.xegfxaccel_iop_xe_early_ready) early_frames=$($S -n debug.xegfxaccel_iop_xe_early_frames)"
echo "== frames $F0 -> $F1  delta=$((F1-F0))   early_signals $E0 -> $E1  delta=$((E1-E0))"
echo "== files"; ls -la "$L" | grep -- "-$TAG" | tail -4
