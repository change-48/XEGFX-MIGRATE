#!/bin/bash
# A-29 §4.5: restart WindowServer with AGDC armed, watch for 75 s
D=$HOME/xegfx-log/A-29
mkdir -p "$D"
rm -f "$D/stream.log"
sudo log stream --predicate 'process == "WindowServer" OR eventMessage CONTAINS "IOPresentment" OR eventMessage CONTAINS "AGDC" OR eventMessage CONTAINS "XEGFX_DISPLAY"' > "$D/stream.log" 2>&1 &
LP=$!
sleep 3
OLD=$(pgrep -x WindowServer)
echo "OLD WS pid=$OLD etime=$(ps -o etime= -p "$OLD" | tr -d ' ')"
echo "kickstart at $(date +%H:%M:%S)"
sudo launchctl kickstart -k system/com.apple.WindowServer
for i in $(seq 1 15); do
  sleep 5
  P=$(pgrep -x WindowServer)
  E=$(ps -o etime= -p "$P" 2>/dev/null | tr -d ' ')
  echo "t=$((i*5))s pid=$P etime=$E"
done
echo "=== final ==="
NEW=$(pgrep -x WindowServer)
echo "NEW WS pid=$NEW"; ps -o etime=,pid= -p "$NEW" 2>/dev/null
kill $LP 2>/dev/null
echo "=== stream greps ==="
echo "transient_not_found=$(grep -c 'transient surface .* not found' "$D/stream.log")"
echo "--- agdc 0x ---"; grep -E 'agdc 0x' "$D/stream.log" | tail -3
echo "--- IOP errors ---"; grep -E 'Unable to (get AGDC|acquire map|locate service)' "$D/stream.log" | tail -6
echo "--- AGDC kernel ---"; grep -E 'AGDC: (UP|slot|no PCI)' "$D/stream.log" | tail -4
echo "--- XEGFX open ---"; grep -E 'open\(\) RELEASED|enableController READY|aperture served' "$D/stream.log" | tail -5
