#!/bin/bash
# A-90 problem B: measure one app launch (click -> window on screen) and what the shader cache did.
# usage: b-run.sh <tag> <AppPath> <hot|cold|coldkeep>
#   hot      : launch as-is
#   cold     : move that app's spvcache dir aside first (learned from this app's own log line), then launch
#   coldkeep : launch without touching the cache (second/third cold data point on an already-empty cache)
set -u
TAG=${1:?tag}; APPPATH=${2:?app path}; MODE=${3:-hot}
T=$HOME/xegfx-log/A-90/tools
NAME=$(basename "$APPPATH" .app)
TTL=90
L=$HOME/xegfx-log/A-90/measure
mkdir -p "$L"

quit_app() {
  osascript -e "quit app \"$NAME\"" >/dev/null 2>&1
  for i in $(seq 1 30); do pgrep -x "$NAME" >/dev/null 2>&1 || return 0; sleep 0.1; done
  pkill -x "$NAME" >/dev/null 2>&1
  sleep 1
}

cache_dir_of() {
  grep -h -o 'spvcache: caching in [^ ]*' "$L"/log-"$TAG"-*.txt 2>/dev/null | tail -1 | awk '{print $4}'
}

quit_app
if [ "$MODE" = cold ] || [ "$MODE" = coldkeep ]; then
  CD=$(cache_dir_of)
  if [ -z "${CD:-}" ] && [ -d "$HOME/Library/Caches/nvmtl/spvcache" ]; then CD="$HOME/Library/Caches/nvmtl/spvcache"; fi
  if [ "$MODE" = cold ] && [ -n "${CD:-}" ] && [ -d "$CD" ]; then
    n=$(ls -1 "$CD" | wc -l | tr -d ' ')
    STAMP=$(date +%H%M%S)
    mv "$CD" "$CD.a90cold-$STAMP" 2>/dev/null || sudo mv "$CD" "$CD.a90cold-$STAMP"
    echo "cold: moved $CD ($n files) aside -> $CD.a90cold-$STAMP"
  fi
  echo "cold: cache dir = ${CD:-unknown}"
fi

# live capture of this process's log (log show over a window is 10x slower)
/usr/bin/log stream --style compact --predicate "process == \"$NAME\"" > "$L/log-$TAG-$MODE.txt" 2>&1 &
LS=$!
sleep 1

T0=$(python3 -c 'import time;print(int(time.time()*1000))')
open -a "$APPPATH"
PID=""
for i in $(seq 1 400); do PID=$(pgrep -x "$NAME" 2>/dev/null | head -1); [ -n "$PID" ] && break; sleep 0.05; done
TPID=$(( $(python3 -c 'import time;print(int(time.time()*1000))') - T0 ))
if [ -z "${PID:-}" ]; then echo "RESULT tag=$TAG mode=$MODE app=$NAME pid=none"; kill $LS 2>/dev/null; exit 1; fi

OUT=$("$T/winwatch2" "$PID" "$TTL" 2>&1)
if echo "$OUT" | grep -q '^WIN'; then
  WE=$(echo "$OUT" | awk '{print $2}'); TWIN=$(( WE - T0 )); GEOM=$(echo "$OUT" | awk '{print $4}')
else
  TWIN="TIMEOUT"; GEOM="-"
fi
sleep 2
kill $LS 2>/dev/null
echo "RESULT tag=$TAG mode=$MODE app=$NAME pid=$PID t_pid_ms=$TPID t_window_ms=$TWIN geom=$GEOM"
grep -h -o 'spvcache: caching in [^ ]*' "$L/log-$TAG-$MODE.txt" | tail -1 | sed 's/^/  DIR /'
grep -E 'library: .*(translated|cache hit)' "$L/log-$TAG-$MODE.txt" | sed -n '1,3p' | sed 's/^/  LOG /'
echo "  COUNT translated=$(grep -c ' translated$' "$L/log-$TAG-$MODE.txt") cache_hit=$(grep -c 'cache hit' "$L/log-$TAG-$MODE.txt")"
