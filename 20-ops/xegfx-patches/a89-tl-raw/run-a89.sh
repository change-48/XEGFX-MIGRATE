#!/bin/bash
# A-89 P1: one 25 s drag, five dumps of the raw frame ring, the frame-time distribution for cross-check.
# usage: run-a89.sh <tag> [sample|nosample]
# Read-only: no switch is written, no sysctl is changed.  The only reason it needs sudo is the frame-times tool.
set -uo pipefail
TAG=${1:?usage: run-a89.sh <tag> [sample|nosample]}
DOSAMPLE=${2:-sample}
L=$HOME/xegfx-log/A-89/measure
S=/usr/sbin/sysctl
mkdir -p "$L"

echo "== admission"
UP=$(awk '{print int($1)}' /proc/uptime 2>/dev/null || sysctl -n kern.boottime >/dev/null && python3 -c 'import time,subprocess,re;print(int(time.time()-float(re.search(r"sec = (\d+)",subprocess.run(["sysctl","-n","kern.boottime"],capture_output=True,text=True).stdout).group(1))))')
LOAD=$(sysctl -n vm.loadavg | awk '{print $2}')
echo "uptime=${UP}s load=$LOAD"
[ "$UP" -lt 150 ] && { echo "REFUSED: booted ${UP}s ago, NSScreen may still be 0x0 (A-88 §5.3)"; exit 1; }
awk -v l="$LOAD" 'BEGIN{exit !(l<3)}' || { echo "REFUSED: load $LOAD >= 3 (A-88 §5.3)"; exit 1; }
[ "$(kmutil showloaded | grep -c XeGfxAccel)" -ge 1 ] || { echo "REFUSED: XeGfxAccel not loaded"; exit 1; }
$S -n debug.xegfxaccel_iop_tl_raw > /dev/null 2>&1 || { echo "REFUSED: debug.xegfxaccel_iop_tl_raw missing -- A-89 accel not installed"; exit 1; }

echo "== state  tag=$TAG"
echo "accel      = $(md5 -q /Library/Extensions/XeGfxAccel.kext/Contents/MacOS/XeGfxAccel | cut -c1-16)"
echo "plugin     = $(md5 -q /Library/GPUBundles/NVMTLDriver.bundle/Contents/MacOS/NVMTLDriver | cut -c1-16)"
echo "flip=$($S -n debug.xegfxaccel_iop_xe_flip) defer=$($S -n debug.xegfxaccel_iop_xe_defer) early=$($S -n debug.xegfxaccel_iop_xe_early) flips=$($S -n debug.xegfxaccel_iop_xe_flips)"
F0=$($S -n debug.xegfxaccel_iop_xe_flips)

~/xegfx-log/A-82/tools/xa82load 25 1100 760 16 -b > "$L/load-$TAG.out" 2>&1 &
lp=$!
sleep 3
sudo -n "$HOME/xegfx-log/A-85/tools/frame-times" 16 220 > "$L/iv-$TAG.txt" 2>&1 &
fp=$!
if [ "$DOSAMPLE" = sample ]; then sleep 10; sudo -n /usr/bin/sample WindowServer 5 -file "$L/sample-$TAG.txt" > /dev/null 2>&1; fi
# five snapshots, 4 s apart: the ring holds 200 frames (~5 s at 40 fps), so consecutive dumps overlap by ~20% and the
# five together cover the whole drag after classify.py dedupes by idx
for i in 1 2 3 4 5; do
  $S -n debug.xegfxaccel_iop_tl_raw > "$L/raw-$TAG-$i.txt" 2>&1
  sleep 4
done
wait $fp 2>/dev/null
kill $lp 2>/dev/null
sleep 1
F1=$($S -n debug.xegfxaccel_iop_xe_flips)

echo "== screen"; head -2 "$L/load-$TAG.out"
echo "== frame times"; grep -E "^(p50|p90|p99|mean|n )" "$L/iv-$TAG.txt" 2>/dev/null | head -8
echo "== latch"; echo "flip_latch_us=$($S -n debug.xegfxaccel_iop_xe_flip_latch_us) latch_timeouts=$($S -n debug.xegfxaccel_iop_xe_latch_timeouts) asleep=$($S -n debug.xegfxaccel_iop_xe_asleep_frames)"
echo "== defer"; echo "queued=$($S -n debug.xegfxaccel_iop_xe_defer_queued) armed=$($S -n debug.xegfxaccel_iop_xe_defer_armed) signals=$($S -n debug.xegfxaccel_iop_xe_defer_signals) dropped=$($S -n debug.xegfxaccel_iop_xe_defer_dropped)"
echo "== frames $F0 -> $F1  delta=$((F1-F0))"
echo "== raw head"; head -2 "$L/raw-$TAG-1.txt"
echo "== raw lines"; wc -l "$L"/raw-"$TAG"-*.txt | tail -1
