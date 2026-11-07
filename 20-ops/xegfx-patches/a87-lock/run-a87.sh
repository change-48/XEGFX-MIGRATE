#!/bin/bash
# A-87: one 25 s drag + frame times + 6 s WindowServer sample, with the oollock switch in the asked state.
# usage: run-a87.sh <tag> <on|off>
set -uo pipefail
TAG=${1:-x}
MODE=${2:-off}
L=$HOME/xegfx-log/A-87/measure
S=/usr/sbin/sysctl
mkdir -p "$L"

if [ "$MODE" = on ]; then sudo sh -c 'echo oollock > /private/tmp/nvmtl-a87'; else sudo rm -f /private/tmp/nvmtl-a87; fi
sleep 1
echo "== state"
echo "mode=$MODE  switch=$(ls /private/tmp/nvmtl-a87 2>/dev/null | wc -l)"
echo "plugin = $(shasum -a256 /Library/GPUBundles/NVMTLDriver.bundle/Contents/MacOS/NVMTLDriver | cut -c1-16)"
echo "flip=$($S -n debug.xegfxaccel_iop_xe_flip 2>/dev/null) defer=$($S -n debug.xegfxaccel_iop_xe_defer 2>/dev/null) wait_idle=$($S -n debug.xegfxaccel_iop_xe_wait_idle 2>/dev/null) flips=$($S -n debug.xegfxaccel_iop_xe_flips 2>/dev/null)"

~/xegfx-log/A-82/tools/xa82load 25 1100 760 16 -b > "$L/load-$TAG.out" 2>&1 &
lp=$!
sleep 3

sudo -n "$HOME/xegfx-log/A-85/tools/frame-times" 16 220 > "$L/iv-$TAG.txt" 2>&1 &
fp=$!

sleep 14
sudo -n /usr/bin/sample WindowServer 6 -file "$L/sample-$TAG.txt" > /dev/null 2>&1

wait $fp 2>/dev/null
kill $lp 2>/dev/null
sleep 1

echo "== screen / load"
head -2 "$L/load-$TAG.out"
echo "== frame times"
grep -E "^(p50|p90|p99|mean|n )" "$L/iv-$TAG.txt" 2>/dev/null | head -8
tail -4 "$L/iv-$TAG.txt"
echo "== files"
ls -la "$L" | grep -- "-$TAG" | tail -4
