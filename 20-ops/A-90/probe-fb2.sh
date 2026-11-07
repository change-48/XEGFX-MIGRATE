#!/bin/bash
F=$HOME/src/NvGfx-5a19a73f36249efb99c44c3f9cb34603ef0c0eb8/kexts/XeGfxIntelFB/intelfb.cpp
echo "=== 文件规模 ==="; wc -l "$F"
echo "=== 类定义 / 继承 ==="; grep -n "^class\|: public IOFramebuffer\|^static .*g[A-Z]" "$F" | head -20
echo "=== sysctl 注册 ==="; grep -n "SYSCTL_" "$F" | head -20
echo "=== start() / probe ==="; grep -n "::start(\|::probe(\|start complete" "$F" | head -10
echo "=== pagingState / wakeGate / deliverFramebuffer ==="; grep -n "pagingState\|fWakeGate\|fServerConnect\|deliverFramebufferNotification\|handleEvent" "$F" | head -20
echo "=== 头文件 ==="; ls "$(dirname "$F")"
