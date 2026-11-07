#!/bin/bash
# A-90 problem B survey: per-process shader cache dirs + plugin cache key
echo "=== spvcache dirs (by process name) ==="
n=0
for d in /var/folders/zz/*/C/*/nvmtl/spvcache; do
  [ -d "$d" ] || continue
  n=$((n+1))
  proc=$(echo "$d" | awk -F/ '{print $(NF-3)}')
  files=$(ls -1 "$d" 2>/dev/null | wc -l | tr -d ' ')
  size=$(du -sk "$d" 2>/dev/null | awk '{print $1}')
  printf '%6s files %8s KB  %s\n' "$files" "$size" "$proc"
done
echo "total dirs: $n"

echo
echo "=== cache dirs elsewhere (nvmtl tree) ==="
ls -la /Library/GPUBundles/nvmtl/ 2>/dev/null | head -20
find /Library/GPUBundles/nvmtl -maxdepth 2 -name '*spv*' 2>/dev/null | head

echo
echo "=== plugin source: cache path key ==="
grep -rn -iE 'spvcache|spv_cache|cache_path|nvmtl_cache' "$HOME/src/NvGfx-xe-2.6.2/plugin" 2>/dev/null | head -30
