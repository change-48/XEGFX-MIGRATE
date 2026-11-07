#!/bin/bash
echo "=== /Applications (前 30) ==="
ls -1 /Applications | head -30
echo
echo "=== System apps 候选（小而常见） ==="
for a in "TextEdit" "Calculator" "Stickies" "Preview" "Notes" "Chess" "Font Book" "Dictionary"; do
  [ -d "/System/Applications/$a.app" ] && echo "sys: $a"
  [ -d "/Applications/$a.app" ] && echo "app: $a"
done
echo
echo "=== WindowServer spvcache (需 sudo) ==="
for d in /var/folders/zz/*/C/WindowServer/nvmtl/spvcache; do
  [ -d "$d" ] && printf '%6s files %9s KB  %s\n' "$(sudo ls -1 "$d" | wc -l | tr -d ' ')" "$(sudo du -sk "$d" | awk '{print $1}')" "$d"
done
echo
echo "=== 翻译耗时日志格式 ==="
grep -rn -E 'nvlog\(".*(translat|MISS|took|ms)' "$HOME/src/NvGfx-xe-2.6.2/plugin/NVMTLLibraryLoad.m" | head -20
