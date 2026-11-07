#!/bin/bash
echo "=== NVMTL_AIRCACHE_DIR 定义 ==="
grep -rn "NVMTL_AIRCACHE_DIR" "$HOME/src/NvGfx-xe-2.6.2/plugin"/*.h "$HOME/src/NvGfx-xe-2.6.2/plugin"/*.m 2>/dev/null | head -5

echo
echo "=== 实际存在的 spvcache 目录 ==="
for pat in "/var/folders/*/*/C/*/nvmtl/spvcache" "$HOME/Library/Caches/nvmtl/spvcache" "$HOME/Library/Containers/*/Data/Library/Caches/nvmtl/spvcache" "$HOME/Library/Group Containers/*/Library/Caches/nvmtl/spvcache"; do
  for d in $pat; do
    [ -d "$d" ] || continue
    files=$(ls -1 "$d" 2>/dev/null | wc -l | tr -d ' ')
    kb=$(du -sk "$d" 2>/dev/null | awk '{print $1}')
    printf '%6s files %9s KB  %s\n' "$files" "$kb" "$d"
  done
done

echo
echo "=== 汇总：按顶层目录 ==="
du -sk /var/folders/*/*/C/*/nvmtl 2>/dev/null | sort -n | tail -15
