#!/bin/bash
# A-83/A-83b 开机卡死 —— 恢复模式下的回滚脚本
# 用法：从恢复 U 盘启动 → 实用工具 → 终端 → 整段粘贴 → 回车
# 只做「复制 / 改名」，不删除任何东西；每步都会打印做了什么。

set +e

# ---------- 1) 找内部数据卷 ----------
D=""
for v in /Volumes/*; do
  [ -d "$v/Users/USER/xegfx-log" ] && D="$v"
done
if [ -z "$D" ]; then
  echo "!! 没找到数据卷。先跑：diskutil list   然后把 - Data 卷 mount 上来："
  echo "   diskutil apfs list | grep -A2 'Data'"
  echo "   diskutil mount diskXsY"
  exit 1
fi
echo "== 数据卷：$D"

# ---------- 2) 找备份目录（优先 A-83 那次，已知是好状态） ----------
BK=""
for c in "$D/Users/USER/xegfx-log/A-83/backup" \
         "$D/Users/USER/xegfx-log/A-83/backup-L1" \
         $(ls -d "$D"/Users/USER/xegfx-log/*/backup 2>/dev/null | tail -3); do
  [ -d "$c" ] && BK="$c" && break
done
echo "== 备份目录：$BK"
ls -la "$BK" 2>/dev/null | head -40

TS=$(date +%H%M%S)

# ---------- 3) 六个 kext 二进制 ----------
for K in XeGfx XeGfxAccel XeGfxIntelFB XeGfxAGDC XeGfxDisplay; do
  SRC=$(ls -t "$BK"/$K.before "$BK"/$K*.before "$BK"/$K.bin* 2>/dev/null | grep -v -E '\.plist' | head -1)
  DST="$D/Library/Extensions/$K.kext/Contents/MacOS/$K"
  if [ -n "$SRC" ] && [ -f "$SRC" ] && [ -f "$DST" ]; then
    cp -p "$DST" "$DST.broken-$TS" 2>/dev/null
    cp -p "$SRC" "$DST" && echo "  [ok] $K  <- $(basename "$SRC")  ($(stat -f%z "$DST") B)"
  else
    echo "  [skip] $K   (src=${SRC:-none})"
  fi
done

# ---------- 4) 内核集合 AuxKC ----------
KCSRC=$(ls -t "$BK"/*.kc 2>/dev/null | head -1)
KC="$D/Library/KernelCollections/AuxiliaryKernelExtensions.kc"
if [ -n "$KCSRC" ]; then
  cp -p "$KC" "$KC.broken-$TS" 2>/dev/null
  cp -p "$KCSRC" "$KC" && echo "  [ok] AuxKC <- $(basename "$KCSRC")  ($(stat -f%z "$KC") B)"
else
  echo "  [skip] AuxKC  (备份里没有 .kc)"
fi

# ---------- 5) 插件（XeMTL 留在盘上会让开机慢到 10 分钟） ----------
PSRC=$(ls -t "$BK"/NVMTLDriver.before "$BK"/NVMTLDriver*.before 2>/dev/null | head -1)
PD="$D/Library/GPUBundles/NVMTLDriver.bundle/Contents/MacOS/NVMTLDriver"
if [ -n "$PSRC" ] && [ -f "$PSRC" ] && [ -f "$PD" ]; then
  cp -p "$PD" "$PD.broken-$TS" 2>/dev/null
  cp -p "$PSRC" "$PD" && echo "  [ok] NVMTLDriver <- $(basename "$PSRC")"
fi
if [ -d "$D/Library/GPUBundles/xemtl" ]; then
  mv "$D/Library/GPUBundles/xemtl" "$D/Library/GPUBundles/xemtl.disabled-$TS" \
    && echo "  [ok] 把 xemtl 桥移开（我们的栈用 nvmtl/，不需要它）"
fi

# ---------- 6) 清掉启动护栏键 ----------
nvram -d xeintelfb-starts 2>/dev/null
nvram -d nmintelfb-starts 2>/dev/null
echo "  [ok] nvram 护栏键已清"

echo
echo "== 完成。核对一下这四样："
echo "   ls -la \"$D/Library/Extensions/XeGfxAccel.kext/Contents/MacOS/\""
echo "   ls -la \"$D/Library/KernelCollections/AuxiliaryKernelExtensions.kc\""
echo "   ls \"$D/Library/Extensions\" | grep -i watchthelight"
echo "   diskutil list | head -20   # 记住内部磁盘的卷名，重启时选它"
echo
echo "== 然后：重启 → 在 OpenCore 菜单里选【内部磁盘】，不要选 U 盘"
