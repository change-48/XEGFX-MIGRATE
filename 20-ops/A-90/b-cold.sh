#!/bin/bash
# A-90 problem B: cold test.  Move the shared shader cache aside, launch two apps, see what they write into a
# fresh one (nothing = they never translate in-process), then put the old cache back without losing new entries.
set -u
L=$HOME/Library/Caches/nvmtl
S=$L/spvcache
B=$S.a90cold-$(date +%H%M%S)
[ -d "$S" ] || { echo "no shared cache at $S"; exit 1; }
before=$(ls -1 "$S" | wc -l | tr -d ' ')
mv "$S" "$B"; echo "moved $before files aside -> $B"
echo "== cold launches =="
bash ~/xegfx-log/A-90/b-run.sh A90 /System/Applications/TextEdit.app coldkeep 2>/dev/null | grep -E "RESULT|cold:"
bash ~/xegfx-log/A-90/b-run.sh A90 /System/Applications/Calculator.app coldkeep 2>/dev/null | grep -E "RESULT|cold:"
echo "== what the fresh cache holds now =="
if [ -d "$S" ]; then echo "new cache: $(ls -1 "$S" | wc -l | tr -d ' ') files"; else echo "new cache: NOT CREATED"; fi
echo "== NVK 设备带起失败的代价（最近一次 Calculator 启动）=="
/usr/bin/log show --last 3m --style compact --predicate 'process == "Calculator" AND eventMessage CONTAINS "NVMTL"' 2>/dev/null \
  | grep -E "initWithAcceleratorPort|driver opened directly|zero physical devices|refusing" | tail -4
echo "== restore: merge the old entries back, keep the new ones =="
mkdir -p "$S"
ditto "$B" "$S" 2>/dev/null || cp -Rp "$B"/* "$S"/ 2>/dev/null
echo "merged: $(ls -1 "$S" | wc -l | tr -d ' ') files (was $before)"
rm -rf "$B"
