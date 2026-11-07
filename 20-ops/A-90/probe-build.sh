#!/bin/bash
T=$HOME/src/NvGfx-5a19a73f36249efb99c44c3f9cb34603ef0c0eb8
echo "=== 可能的构建脚本 ==="
find "$T/kexts/XeGfxIntelFB" "$T" -maxdepth 2 -name 'Makefile*' -o -maxdepth 2 -name '*.sh' -o -maxdepth 2 -name 'build*' 2>/dev/null | head -20
echo "=== 历史日志里的编译命令 ==="
grep -rh "intelfb" "$HOME"/xegfx-log/*/*.sh "$HOME"/xegfx-log/*.txt 2>/dev/null | grep -iE "clang|ld |kextbuild" | head -10
echo "=== 二进制里的路径/编译痕迹 ==="
strings -a /Library/Extensions/XeGfxIntelFB.kext/Contents/MacOS/XeGfxIntelFB | grep -iE "clang|\.cpp|KDK|CommandLineTools" | head -10
echo "=== 现在时间/开机状态 ==="
date +%H:%M:%S; tail -5 /var/log/xegfx-bringup.log
